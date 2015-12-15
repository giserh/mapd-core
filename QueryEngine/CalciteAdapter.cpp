#include "CalciteAdapter.h"

#include "../Parser/ParserNode.h"
#include "../Shared/sqldefs.h"
#include "../Shared/sqltypes.h"

#include <glog/logging.h>
#include <rapidjson/document.h>

#include <set>

namespace {

SQLOps to_bin_op(const std::string& bin_op_str) {
  if (bin_op_str == std::string(">")) {
    return kGT;
  }
  CHECK(false);
  return kEQ;
}

SQLAgg to_agg_kind(const std::string& agg_name) {
  if (agg_name == std::string("COUNT")) {
    return kCOUNT;
  }
  CHECK(false);
  return kCOUNT;
}

SQLTypes to_sql_type(const std::string& type_name) {
  if (type_name == std::string("BIGINT")) {
    return kBIGINT;
  }
  CHECK(false);
  return kNULLT;
}

class CalciteAdapter {
 public:
  CalciteAdapter(const Catalog_Namespace::Catalog& cat) : cat_(cat) {}

  std::shared_ptr<Analyzer::Expr> getExprFromNode(const rapidjson::Value& expr, const TableDescriptor* td) {
    if (expr.IsObject() && expr.HasMember("op")) {
      return translateBinOp(expr, td);
    }
    if (expr.IsObject() && expr.HasMember("input")) {
      return translateColRef(expr, td);
    }
    if (expr.IsObject() && expr.HasMember("agg")) {
      return translateAggregate(expr, td);
    }
    if (expr.IsInt()) {
      return translateIntLiteral(expr);
    }
    CHECK(false);
    return nullptr;
  }

  std::shared_ptr<Analyzer::Expr> translateBinOp(const rapidjson::Value& expr, const TableDescriptor* td) {
    const auto bin_op_str = expr["op"].GetString();
    const auto& operands = expr["operands"];
    CHECK(operands.IsArray());
    CHECK_EQ(unsigned(2), operands.Size());
    const auto lhs = getExprFromNode(operands[0], td);
    const auto rhs = getExprFromNode(operands[1], td);
    return Parser::OperExpr::normalize(to_bin_op(bin_op_str), kONE, lhs, rhs);
  }

  std::shared_ptr<Analyzer::Expr> translateColRef(const rapidjson::Value& expr, const TableDescriptor* td) {
    const int col_id = expr["input"].GetInt();
    used_columns_.insert(col_id);
    const auto cd = cat_.getMetadataForColumn(td->tableId, col_id);
    CHECK(cd);
    return std::make_shared<Analyzer::ColumnVar>(cd->columnType, td->tableId, col_id, 0);
  }

  std::shared_ptr<Analyzer::Expr> translateAggregate(const rapidjson::Value& expr, const TableDescriptor* td) {
    CHECK(expr.IsObject() && expr.HasMember("type"));
    const auto& expr_type = expr["type"];
    CHECK(expr_type.IsObject());
    SQLTypeInfo agg_ti(to_sql_type(expr_type["type"].GetString()), expr_type["nullable"].GetBool());
    const auto agg_name = expr["agg"].GetString();
    return std::make_shared<Analyzer::AggExpr>(agg_ti, to_agg_kind(agg_name), nullptr, false);
  }

  std::shared_ptr<Analyzer::Expr> translateIntLiteral(const rapidjson::Value& expr) {
    return Parser::IntLiteral::analyzeValue(expr.GetInt64());
  }

  std::list<int> getUsedColumnList() const {
    std::list<int> used_column_list;
    for (const int used_col : used_columns_) {
      used_column_list.push_back(used_col);
    }
    return used_column_list;
  }

  const TableDescriptor* getTableFromScanNode(const rapidjson::Value& scan_ra) const {
    const auto& table_info = scan_ra["table"];
    CHECK(table_info.IsArray());
    CHECK_EQ(unsigned(3), table_info.Size());
    const auto td = cat_.getMetadataForTable(table_info[2].GetString());
    CHECK(td);
    return td;
  }

 private:
  std::set<int> used_columns_;
  const Catalog_Namespace::Catalog& cat_;
};

void collect_target_entries(std::vector<Analyzer::TargetEntry*>& agg_targets,
                            std::vector<Analyzer::TargetEntry*>& scan_targets,
                            const rapidjson::Value& proj_nodes,
                            const rapidjson::Value& agg_nodes,
                            CalciteAdapter& calcite_adapter,
                            const TableDescriptor* td) {
  CHECK(proj_nodes.IsArray());
  for (size_t i = 0; i < proj_nodes.Size(); ++i) {
    const auto proj_expr = calcite_adapter.getExprFromNode(proj_nodes[i], td);
    scan_targets.push_back(new Analyzer::TargetEntry("", proj_expr, false));
    agg_targets.push_back(new Analyzer::TargetEntry("", proj_expr, false));
  }
  CHECK(agg_nodes.IsArray());
  for (size_t i = 0; i < agg_nodes.Size(); ++i) {
    auto agg_expr = calcite_adapter.getExprFromNode(agg_nodes[i], td);
    agg_targets.push_back(new Analyzer::TargetEntry("", agg_expr, false));
  }
}

void collect_groupby(const rapidjson::Value& group_nodes,
                     const std::vector<Analyzer::TargetEntry*>& agg_targets,
                     std::list<std::shared_ptr<Analyzer::Expr>>& groupby_exprs) {
  CHECK(group_nodes.IsArray());
  for (size_t i = 0; i < group_nodes.Size(); ++i) {
    const int target_idx = group_nodes[i].GetInt();
    groupby_exprs.push_back(agg_targets[target_idx]->get_expr()->deep_copy());
  }
}

}  // namespace

Planner::RootPlan* translate_query(const std::string& query, const Catalog_Namespace::Catalog& cat) {
  rapidjson::Document query_ast;
  query_ast.Parse(query.c_str());
  CHECK(!query_ast.HasParseError());
  CHECK(query_ast.IsObject());
  const auto& rels = query_ast["rels"];
  CHECK(rels.IsArray());
  CHECK_EQ(unsigned(4), rels.Size());
  const auto& scan_ra = rels[0];
  CHECK(scan_ra.IsObject());
  CHECK_EQ(std::string("LogicalTableScan"), scan_ra["relOp"].GetString());
  const auto& filter_ra = rels[1];
  CHECK(filter_ra.IsObject());
  CalciteAdapter calcite_adapter(cat);
  auto td = calcite_adapter.getTableFromScanNode(scan_ra);
  const auto filter_expr = calcite_adapter.getExprFromNode(filter_ra["condition"], td);
  const auto& project_ra = rels[2];
  const auto& proj_nodes = project_ra["exprs"];
  const auto& agg_nodes = rels[3]["aggs"];
  const auto& group_nodes = rels[3]["group"];
  CHECK(proj_nodes.IsArray());
  std::vector<Analyzer::TargetEntry*> agg_targets;
  std::vector<Analyzer::TargetEntry*> scan_targets;
  std::list<std::shared_ptr<Analyzer::Expr>> groupby_exprs;
  collect_target_entries(agg_targets, scan_targets, proj_nodes, agg_nodes, calcite_adapter, td);
  collect_groupby(group_nodes, agg_targets, groupby_exprs);
  std::list<std::shared_ptr<Analyzer::Expr>> q;
  std::list<std::shared_ptr<Analyzer::Expr>> sq{filter_expr};
  auto scan_plan =
      new Planner::Scan(scan_targets, q, 0., nullptr, sq, td->tableId, calcite_adapter.getUsedColumnList());
  auto agg_plan = new Planner::AggPlan(agg_targets, 0., scan_plan, groupby_exprs);
  auto root_plan = new Planner::RootPlan(agg_plan, kSELECT, td->tableId, {}, cat, 0, 0);
  root_plan->print();
  puts("");
  return root_plan;
}