#include "duckdb/optimizer/index_join.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"

#include "duckdb/parser/expression/comparison_expression.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_index_scan.hpp"

#include "duckdb/storage/data_table.hpp"
using namespace duckdb;
using namespace std;


unique_ptr<LogicalOperator> IndexJoin::Optimize(unique_ptr<LogicalOperator> op) {
    // We need to check if the current node is a Join and if either the left side or the right side is a get
    if (op->type == LogicalOperatorType::COMPARISON_JOIN && (op->children[0]->type == LogicalOperatorType::GET ||op->children[1]->type == LogicalOperatorType::GET) ) {
        return TransformJoinToIndexJoin(move(op));
    }
    for (auto &child : op->children) {
        child = Optimize(move(child));
    }
    return op;
}

unique_ptr<LogicalOperator> IndexJoin::TransformJoinToIndexJoin(unique_ptr<LogicalOperator> op) {
    assert(op->type == LogicalOperatorType::COMPARISON_JOIN);
    auto &join = (LogicalComparisonJoin &)*op;
    //FIXME : If both children are get and have indexes than we need to check which table is smaller
    //FIXME : For now only checking left child
    auto get = (LogicalGet *)op->children[0].get();

    if (!get->table) {
        return op;
    }

    auto &storage = *get->table->storage;

    if (storage.indexes.size() == 0) {
        // no indexes on the table, can't rewrite
        return op;
    }

    // check all the indexes
    for (size_t j = 0; j < storage.indexes.size(); j++) {
        auto &index = storage.indexes[j];

        //		assert(index->unbound_expressions.size() == 1);
        // first rewrite the index expression so the ColumnBindings align with the column bindings of the current table
        if (index->unbound_expressions.size() > 1)
            continue;
        auto index_expression = index->unbound_expressions[0]->Copy();
        bool rewrite_possible = true;
        RewriteIndexExpression(*index, *get, *index_expression, rewrite_possible);
        if (!rewrite_possible) {
            // could not rewrite!
            continue;
        }

        Value low_value, high_value, equal_value;
        // try to find a matching index for any of the filter expressions

        //FIXME need to check join expressions
        auto expr = join.expressions[0].get();
        auto low_comparison_type = expr->type;
        auto high_comparison_type = expr->type;
        for (index_t i = 0; i < join.expressions.size(); i++) {
            expr = join.expressions[i].get();
            // create a matcher for a comparison with a constant
            ComparisonExpressionMatcher matcher;
            // match on a comparison type
            matcher.expr_type = make_unique<ComparisonExpressionTypeMatcher>();
            // match on a constant comparison with the indexed expression
            matcher.matchers.push_back(make_unique<ExpressionEqualityMatcher>(index_expression.get()));
            matcher.matchers.push_back(make_unique<ConstantExpressionMatcher>());

            matcher.policy = SetMatcher::Policy::UNORDERED;

            vector<Expression *> bindings;
            if (matcher.Match(expr, bindings)) {
                // range or equality comparison with constant value
                // we can use our index here
                // bindings[0] = the expression
                // bindings[1] = the index expression
                // bindings[2] = the constant
                auto comparison = (BoundComparisonExpression *)bindings[0];
                assert(bindings[0]->GetExpressionClass() == ExpressionClass::BOUND_COMPARISON);
                assert(bindings[2]->type == ExpressionType::VALUE_CONSTANT);

                auto constant_value = ((BoundConstantExpression *)bindings[2])->value;
                auto comparison_type = comparison->type;
                if (comparison->left->type == ExpressionType::VALUE_CONSTANT) {
                    // the expression is on the right side, we flip them around
                    comparison_type = FlipComparisionExpression(comparison_type);
                }
                if (comparison_type == ExpressionType::COMPARE_EQUAL) {
                    // equality value
                    // equality overrides any other bounds so we just break here
                    equal_value = constant_value;
                    break;
                } else if (comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
                           comparison_type == ExpressionType::COMPARE_GREATERTHAN) {
                    // greater than means this is a lower bound
                    low_value = constant_value;
                    low_comparison_type = comparison_type;
                } else {
                    // smaller than means this is an upper bound
                    high_value = constant_value;
                    high_comparison_type = comparison_type;
                }
            }
        }
        if (!equal_value.is_null || !low_value.is_null || !high_value.is_null) {
            auto logical_index_scan = make_unique<LogicalIndexScan>(*get->table, *get->table->storage, *index,
                                                                    get->column_ids, get->table_index);
            if (!equal_value.is_null) {
                logical_index_scan->equal_value = equal_value;
                logical_index_scan->equal_index = true;
            }
            if (!low_value.is_null) {
                logical_index_scan->low_value = low_value;
                logical_index_scan->low_index = true;
                logical_index_scan->low_expression_type = low_comparison_type;
            }
            if (!high_value.is_null) {
                logical_index_scan->high_value = high_value;
                logical_index_scan->high_index = true;
                logical_index_scan->high_expression_type = high_comparison_type;
            }
            op->children[0] = move(logical_index_scan);
            break;
        }
    }
    return op;
}
