/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/config.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace ExpressionTests {

using boost::intrusive_ptr;
using std::initializer_list;
using std::numeric_limits;
using std::pair;
using std::set;
using std::sort;
using std::string;
using std::vector;
using std::list;

/**
 * Takes the name of an expression as its first argument and a list of pairs of arguments and
 * expected results as its second argument, and asserts that for the given expression the arguments
 * evaluate to the expected results.
 */
static void assertExpectedResults(string expression,
                                  initializer_list<pair<vector<Value>, Value>> operations) {
    for (auto&& op : operations) {
        try {
            VariablesIdGenerator idGenerator;
            VariablesParseState vps(&idGenerator);
            const BSONObj obj = BSON(expression << Value(op.first));
            Value result =
                Expression::parseExpression(obj.firstElement(), vps)->evaluate(Document());
            ASSERT_EQUALS(op.second, result);
            ASSERT_EQUALS(op.second.getType(), result.getType());
        } catch (...) {
            log() << "failed with arguments: " << Value(op.first);
            throw;
        }
    }
}

/** Convert BSONObj to a BSONObj with our $const wrappings. */
static BSONObj constify(const BSONObj& obj, bool parentIsArray = false) {
    BSONObjBuilder bob;
    for (BSONObjIterator itr(obj); itr.more(); itr.next()) {
        BSONElement elem = *itr;
        if (elem.type() == Object) {
            bob << elem.fieldName() << constify(elem.Obj(), false);
        } else if (elem.type() == Array && !parentIsArray) {
            // arrays within arrays are treated as constant values by the real
            // parser
            bob << elem.fieldName() << BSONArray(constify(elem.Obj(), true));
        } else if (str::equals(elem.fieldName(), "$const") ||
                   (elem.type() == mongo::String && elem.valuestrsafe()[0] == '$')) {
            bob.append(elem);
        } else {
            bob.append(elem.fieldName(), BSON("$const" << elem));
        }
    }
    return bob.obj();
}

/** Check binary equality, ensuring use of the same numeric types. */
static void assertBinaryEqual(const BSONObj& expected, const BSONObj& actual) {
    ASSERT_EQUALS(expected, actual);
    ASSERT(expected.binaryEqual(actual));
}

/** Convert Value to a wrapped BSONObj with an empty string field name. */
static BSONObj toBson(const Value& value) {
    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

/** Convert Expression to BSON. */
static BSONObj expressionToBson(const intrusive_ptr<Expression>& expression) {
    return BSON("" << expression->serialize(false)).firstElement().embeddedObject().getOwned();
}

/** Convert Document to BSON. */
static BSONObj toBson(const Document& document) {
    return document.toBson();
}

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj) {
    return Document(obj);
}

/** Create a Value from a BSONObj. */
Value valueFromBson(BSONObj obj) {
    BSONElement element = obj.firstElement();
    return Value(element);
}


class ExpressionBaseTest : public unittest::Test {
public:
    void addOperand(intrusive_ptr<ExpressionNary> expr, Value arg) {
        expr->addOperand(ExpressionConstant::create(arg));
    }
};

class ExpressionNaryTestOneArg : public ExpressionBaseTest {
public:
    virtual void assertEvaluates(Value input, Value output) {
        addOperand(_expr, input);
        ASSERT_EQUALS(output, _expr->evaluate(Document()));
        ASSERT_EQUALS(output.getType(), _expr->evaluate(Document()).getType());
    }

    intrusive_ptr<ExpressionNary> _expr;
};

/* ------------------------- NaryExpression -------------------------- */

/** A dummy child of ExpressionNary used for testing. */
class Testable : public ExpressionNary {
public:
    virtual Value evaluateInternal(Variables* vars) const {
        // Just put all the values in a list.
        // By default, this is not associative/commutative so the results will change if
        // instantiated as commutative or associative and operations are reordered.
        vector<Value> values;
        for (ExpressionVector::const_iterator i = vpOperand.begin(); i != vpOperand.end(); ++i) {
            values.push_back((*i)->evaluateInternal(vars));
        }
        return Value(values);
    }

    virtual const char* getOpName() const {
        return "$testable";
    }

    virtual bool isAssociative() const {
        return _isAssociative;
    }

    virtual bool isCommutative() const {
        return _isCommutative;
    }

    static intrusive_ptr<Testable> create(bool associative, bool commutative) {
        return new Testable(associative, commutative);
    }

private:
    Testable(bool isAssociative, bool isCommutative)
        : _isAssociative(isAssociative), _isCommutative(isCommutative) {}
    bool _isAssociative;
    bool _isCommutative;
};

class ExpressionNaryTest : public unittest::Test {
public:
    virtual void setUp() override {
        _notAssociativeNorCommutative = Testable::create(false, false);
        _associativeOnly = Testable::create(true, false);
        _associativeAndCommutative = Testable::create(true, true);
    }

protected:
    void assertDependencies(const intrusive_ptr<Testable>& expr,
                            const BSONArray& expectedDependencies) {
        DepsTracker dependencies;
        expr->addDependencies(&dependencies);
        BSONArrayBuilder dependenciesBson;
        for (set<string>::const_iterator i = dependencies.fields.begin();
             i != dependencies.fields.end();
             ++i) {
            dependenciesBson << *i;
        }
        ASSERT_EQUALS(expectedDependencies, dependenciesBson.arr());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }

    void assertContents(const intrusive_ptr<Testable>& expr, const BSONArray& expectedContents) {
        ASSERT_EQUALS(constify(BSON("$testable" << expectedContents)), expressionToBson(expr));
    }

    void addOperandArrayToExpr(const intrusive_ptr<Testable>& expr, const BSONArray& operands) {
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        BSONObjIterator i(operands);
        while (i.more()) {
            BSONElement element = i.next();
            expr->addOperand(Expression::parseOperand(element, vps));
        }
    }

    intrusive_ptr<Testable> _notAssociativeNorCommutative;
    intrusive_ptr<Testable> _associativeOnly;
    intrusive_ptr<Testable> _associativeAndCommutative;
};

TEST_F(ExpressionNaryTest, AddedConstantOperandIsSerialized) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(Value(9)));
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY(9));
}

TEST_F(ExpressionNaryTest, AddedFieldPathOperandIsSerialized) {
    _notAssociativeNorCommutative->addOperand(ExpressionFieldPath::create("ab.c"));
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY("$ab.c"));
}

TEST_F(ExpressionNaryTest, ValidateEmptyDependencies) {
    assertDependencies(_notAssociativeNorCommutative, BSONArray());
}

TEST_F(ExpressionNaryTest, ValidateConstantExpressionDependency) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(Value(1)));
    assertDependencies(_notAssociativeNorCommutative, BSONArray());
}

TEST_F(ExpressionNaryTest, ValidateFieldPathExpressionDependency) {
    _notAssociativeNorCommutative->addOperand(ExpressionFieldPath::create("ab.c"));
    assertDependencies(_notAssociativeNorCommutative, BSON_ARRAY("ab.c"));
}

TEST_F(ExpressionNaryTest, ValidateObjectExpressionDependency) {
    BSONObj spec = BSON("" << BSON("a"
                                   << "$x"
                                   << "q"
                                   << "$r"));
    BSONElement specElement = spec.firstElement();
    Expression::ObjectCtx ctx(Expression::ObjectCtx::DOCUMENT_OK);
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);
    _notAssociativeNorCommutative->addOperand(
        Expression::parseObject(specElement.Obj(), &ctx, vps));
    assertDependencies(_notAssociativeNorCommutative,
                       BSON_ARRAY("r"
                                  << "x"));
}

TEST_F(ExpressionNaryTest, SerializationToBsonObj) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(Value(5)));
    ASSERT_EQUALS(BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5)))),
                  BSON("foo" << _notAssociativeNorCommutative->serialize(false)));
}

TEST_F(ExpressionNaryTest, SerializationToBsonArr) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(Value(5)));
    ASSERT_EQUALS(constify(BSON_ARRAY(BSON("$testable" << BSON_ARRAY(5)))),
                  BSON_ARRAY(_notAssociativeNorCommutative->serialize(false)));
}

// Verify that the internal operands are optimized
TEST_F(ExpressionNaryTest, InternalOperandOptimizationIsDone) {
    BSONArray spec = BSON_ARRAY(BSON("$and" << BSONArray()) << "$abc");
    addOperandArrayToExpr(_notAssociativeNorCommutative, spec);
    assertContents(_notAssociativeNorCommutative, spec);
    ASSERT(_notAssociativeNorCommutative == _notAssociativeNorCommutative->optimize());
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY(true << "$abc"));
}

// Verify that if all the operands are constants, the expression is replaced
// by a constant value equivalent to the expression applied to the operands.
TEST_F(ExpressionNaryTest, AllConstantOperandOptimization) {
    BSONArray spec = BSON_ARRAY(1 << 2);
    addOperandArrayToExpr(_notAssociativeNorCommutative, spec);
    assertContents(_notAssociativeNorCommutative, spec);
    intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative != optimized);
    ASSERT_EQUALS(BSON("$const" << BSON_ARRAY(1 << 2)), expressionToBson(optimized));
}

// Verify that the optimization of grouping constant and non-constant operands
// and then applying the expression to the constant operands to reduce them to
// one constant operand is only applied if the expression is associative and
// commutative.
TEST_F(ExpressionNaryTest, GroupingOptimizationOnNotCommutativeNorAssociative) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_notAssociativeNorCommutative, spec);
    assertContents(_notAssociativeNorCommutative, spec);
    intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative == optimized);
    assertContents(_notAssociativeNorCommutative, spec);
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyFrontOperands) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyMiddleOperands) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyBackOperands) {
    BSONArray spec = BSON_ARRAY("$path" << 55 << 66);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsFront) {
    BSONArray spec = BSON_ARRAY(55 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY(55 << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsMiddle) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << "$path2");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << 55 << "$path2"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsBack) {
    BSONArray spec = BSON_ARRAY("$path" << 55);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << 55));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnCommutativeAndAssociative) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_associativeAndCommutative, spec);
    assertContents(_associativeAndCommutative, spec);
    intrusive_ptr<Expression> optimized = _associativeAndCommutative->optimize();
    ASSERT(_associativeAndCommutative == optimized);
    assertContents(_associativeAndCommutative, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, FlattenOptimizationNotDoneOnOtherExpressionsForAssociativeExpressions) {
    BSONArray spec = BSON_ARRAY(66 << "$path" << BSON("$sum" << BSON_ARRAY("$path" << 2)));
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, spec);
}

TEST_F(ExpressionNaryTest, FlattenOptimizationNotDoneOnSameButNotAssociativeExpression) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(false, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    assertContents(_associativeOnly, spec);
}

// Test that if there is an expression of the same type in a non-commutative nor associative
// expression, the inner expression is not expanded.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnNotCommutativeNorAssociative) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(false, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _notAssociativeNorCommutative->addOperand(innerOperand);

    addOperandArrayToExpr(_notAssociativeNorCommutative, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_notAssociativeNorCommutative, spec);
    intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative == optimized);

    assertContents(_notAssociativeNorCommutative, spec);
}

// Test that if there is an expression of the same type as the first operand
// in a non-commutative but associative expression, the inner expression is expanded.
// Also, there shouldn't be any grouping of the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ 100, "$path1", 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyFrontOperandNoGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << 99 << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type as the first operand
// in a non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1", 101] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ 100, "$path1", [101, 99], "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyFrontOperandAndGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << BSON_ARRAY(101 << 99) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the middle of the operands
// in a non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should not be applied to the operands.
// {"$testable" : [ 200, "$path3", { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is
// optimized to: {"$testable" : [ 200, "$path3", 100, "$path1", 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyMiddleOperandNoGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3"));
    specBuilder << 200 << "$path3";

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(200 << "$path3" << 100 << "$path1" << 99 << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the middle of the operands
// in a non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ 200, "$path3", 201 { "$testable" : [ 100, "$path1", 101] }, 99, "$path2"] } is
// optimized to: {"$testable" : [ 200, "$path3", [201, 100], "$path1", [101, 99], "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyMiddleOperandAndGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3" << 201));
    specBuilder << 200 << "$path3" << 201;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(
        200 << "$path3" << BSON_ARRAY(201 << 100) << "$path1" << BSON_ARRAY(101 << 99) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the back of the operands in a
// non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should not be applied to the operands.
// {"$testable" : [ 200, "$path3", { "$testable" : [ 100, "$path1"] }] } is
// optimized to: {"$testable" : [ 200, "$path3", 100, "$path1"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyBackOperandNoGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3"));
    specBuilder << 200 << "$path3";

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(200 << "$path3" << 100 << "$path1");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the back of the operands in a
// non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ 200, "$path3", 201, { "$testable" : [ 100, "$path1", 101] }] } is
// optimized to: {"$testable" : [ 200, "$path3", [201, 100], "$path1", 101] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyBackOperandAndGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3" << 201));
    specBuilder << 200 << "$path3" << 201;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent =
        BSON_ARRAY(200 << "$path3" << BSON_ARRAY(201 << 100) << "$path1" << 101);
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there are two consecutive inner expressions of the same type in a non-commutative
// but associative expression, both expressions are correctly flattened.
// Partial collapsing optimization should not be applied to the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, { "$testable" : [ 200, "$path2"] }] } is
// optimized to: {"$testable" : [ 100, "$path1", 200, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenConsecutiveInnerOperandsOptimizationOnAssociativeOnlyNoGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    intrusive_ptr<Testable> innerOperand2 = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand2, BSON_ARRAY(200 << "$path2"));
    specBuilder.append(expressionToBson(innerOperand2));
    _associativeOnly->addOperand(innerOperand2);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << 200 << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there are two consecutive inner expressions of the same type in a non-commutative
// but associative expression, both expressions are correctly flattened.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1", 101] }, { "$testable" : [ 200, "$path2"] }] }
// is optimized to: {"$testable" : [ 100, "$path1", [ 101, 200], "$path2"] }
TEST_F(ExpressionNaryTest, FlattenConsecutiveInnerOperandsOptimizationOnAssociativeAndGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    intrusive_ptr<Testable> innerOperand2 = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand2, BSON_ARRAY(200 << "$path2"));
    specBuilder.append(expressionToBson(innerOperand2));
    _associativeOnly->addOperand(innerOperand2);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << BSON_ARRAY(101 << 200) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that inner expressions are correctly flattened and constant operands re-arranged and
// collapsed when using a commutative and associative expression.
// {"$testable" : [ 200, "$path3", 201, { "$testable" : [ 100, "$path1", 101] }, 99, "$path2"] } is
// optimized to: {"$testable" : [ "$path3", "$path1", "$path2", [200, 201, [ 100, 101], 99] ] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnCommutativeAndAssociative) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeAndCommutative, BSON_ARRAY(200 << "$path3" << 201));
    specBuilder << 200 << "$path3" << 201;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, true);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeAndCommutative->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeAndCommutative, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeAndCommutative, spec);
    intrusive_ptr<Expression> optimized = _associativeAndCommutative->optimize();
    ASSERT(_associativeAndCommutative == optimized);

    BSONArray expectedContent = BSON_ARRAY("$path3"
                                           << "$path1"
                                           << "$path2"
                                           << BSON_ARRAY(200 << 201 << BSON_ARRAY(100 << 101)
                                                             << 99));
    assertContents(_associativeAndCommutative, expectedContent);
}

/* ------------------------- ExpressionCeil -------------------------- */

class ExpressionCeilTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionCeil();
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionCeilTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(numeric_limits<int>::min()), Value(numeric_limits<int>::min()));
    assertEvaluates(Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max()));
}

TEST_F(ExpressionCeilTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(numeric_limits<long long>::min()),
                    Value(numeric_limits<long long>::min()));
    assertEvaluates(Value(numeric_limits<long long>::max()),
                    Value(numeric_limits<long long>::max()));
}

TEST_F(ExpressionCeilTest, DoubleArg) {
    assertEvaluates(Value(2.0), Value(2.0));
    assertEvaluates(Value(-2.0), Value(-2.0));
    assertEvaluates(Value(0.9), Value(1.0));
    assertEvaluates(Value(0.1), Value(1.0));
    assertEvaluates(Value(-1.2), Value(-1.0));
    assertEvaluates(Value(-1.7), Value(-1.0));

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // ceil should just preserve the number).
    double largerThanLong = numeric_limits<long long>::max() * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEvaluates(Value(smallerThanLong), Value(smallerThanLong));
}

TEST_F(ExpressionCeilTest, DecimalArg) {
    assertEvaluates(Value(Decimal128("2")), Value(Decimal128("2.0")));
    assertEvaluates(Value(Decimal128("-2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("0.9")), Value(Decimal128("1.0")));
    assertEvaluates(Value(Decimal128("0.1")), Value(Decimal128("1.0")));
    assertEvaluates(Value(Decimal128("-1.2")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("-1.7")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("1234567889.000000000000000000000001")),
                    Value(Decimal128("1234567890")));
    assertEvaluates(Value(Decimal128("-99999999999999999999999999999.99")),
                    Value(Decimal128("-99999999999999999999999999999.00")));
    assertEvaluates(Value(Decimal128("3.4E-6000")), Value(Decimal128("1")));
}

TEST_F(ExpressionCeilTest, NullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionFloor -------------------------- */

class ExpressionFloorTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionFloor();
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionFloorTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(numeric_limits<int>::min()), Value(numeric_limits<int>::min()));
    assertEvaluates(Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max()));
}

TEST_F(ExpressionFloorTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(numeric_limits<long long>::min()),
                    Value(numeric_limits<long long>::min()));
    assertEvaluates(Value(numeric_limits<long long>::max()),
                    Value(numeric_limits<long long>::max()));
}

TEST_F(ExpressionFloorTest, DoubleArg) {
    assertEvaluates(Value(2.0), Value(2.0));
    assertEvaluates(Value(-2.0), Value(-2.0));
    assertEvaluates(Value(0.9), Value(0.0));
    assertEvaluates(Value(0.1), Value(0.0));
    assertEvaluates(Value(-1.2), Value(-2.0));
    assertEvaluates(Value(-1.7), Value(-2.0));

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // floor should just preserve the number).
    double largerThanLong = numeric_limits<long long>::max() * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEvaluates(Value(smallerThanLong), Value(smallerThanLong));
}

TEST_F(ExpressionFloorTest, DecimalArg) {
    assertEvaluates(Value(Decimal128("2")), Value(Decimal128("2.0")));
    assertEvaluates(Value(Decimal128("-2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("0.9")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("0.1")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("-1.2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("-1.7")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("1234567890.000000000000000000000001")),
                    Value(Decimal128("1234567890")));
    assertEvaluates(Value(Decimal128("-99999999999999999999999999999.99")),
                    Value(Decimal128("-100000000000000000000000000000")));
    assertEvaluates(Value(Decimal128("3.4E-6000")), Value(Decimal128("0")));
}

TEST_F(ExpressionFloorTest, NullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------ ExpressionRange --------------------------- */

TEST(ExpressionRangeTest, ComputesStandardRange) {
    assertExpectedResults("$range", {{{Value(0), Value(3)}, Value(BSON_ARRAY(0 << 1 << 2))}});
}

TEST(ExpressionRangeTest, ComputesRangeWithStep) {
    assertExpectedResults("$range",
                          {{{Value(0), Value(6), Value(2)}, Value(BSON_ARRAY(0 << 2 << 4))}});
}

TEST(ExpressionRangeTest, ComputesReverseRange) {
    assertExpectedResults("$range",
                          {{{Value(0), Value(-3), Value(-1)}, Value(BSON_ARRAY(0 << -1 << -2))}});
}

TEST(ExpressionRangeTest, ComputesRangeWithPositiveAndNegative) {
    assertExpectedResults("$range",
                          {{{Value(-2), Value(3)}, Value(BSON_ARRAY(-2 << -1 << 0 << 1 << 2))}});
}

TEST(ExpressionRangeTest, ComputesEmptyRange) {
    assertExpectedResults("$range",
                          {{{Value(-2), Value(3), Value(-1)}, Value(std::vector<Value>())}});
}

TEST(ExpressionRangeTest, ComputesRangeWithSameStartAndEnd) {
    assertExpectedResults("$range", {{{Value(20), Value(20)}, Value(std::vector<Value>())}});
}

TEST(ExpressionRangeTest, ComputesRangeWithLargeNegativeStep) {
    assertExpectedResults("$range",
                          {{{Value(3), Value(-5), Value(-3)}, Value(BSON_ARRAY(3 << 0 << -3))}});
}

/* ------------------------ ExpressionReverseArray -------------------- */

TEST(ExpressionReverseArrayTest, ReversesNormalArray) {
    assertExpectedResults("$reverseArray",
                          {{{Value(BSON_ARRAY(1 << 2 << 3))}, Value(BSON_ARRAY(3 << 2 << 1))}});
}

TEST(ExpressionReverseArrayTest, ReversesEmptyArray) {
    assertExpectedResults("$reverseArray",
                          {{{Value(std::vector<Value>())}, Value(std::vector<Value>())}});
}

TEST(ExpressionReverseArrayTest, ReversesOneElementArray) {
    assertExpectedResults("$reverseArray", {{{Value(BSON_ARRAY(1))}, Value(BSON_ARRAY(1))}});
}

TEST(ExpressionReverseArrayTest, ReturnsNullWithNullishInput) {
    assertExpectedResults(
        "$reverseArray",
        {{{Value(BSONNULL)}, Value(BSONNULL)}, {{Value(BSONUndefined)}, Value(BSONNULL)}});
}

/* ------------------------- ExpressionTrunc -------------------------- */

class ExpressionTruncTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionTrunc();
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionTruncTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(numeric_limits<int>::min()), Value(numeric_limits<int>::min()));
    assertEvaluates(Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max()));
}

TEST_F(ExpressionTruncTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(numeric_limits<long long>::min()),
                    Value(numeric_limits<long long>::min()));
    assertEvaluates(Value(numeric_limits<long long>::max()),
                    Value(numeric_limits<long long>::max()));
}

TEST_F(ExpressionTruncTest, DoubleArg) {
    assertEvaluates(Value(2.0), Value(2.0));
    assertEvaluates(Value(-2.0), Value(-2.0));
    assertEvaluates(Value(0.9), Value(0.0));
    assertEvaluates(Value(0.1), Value(0.0));
    assertEvaluates(Value(-1.2), Value(-1.0));
    assertEvaluates(Value(-1.7), Value(-1.0));

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // should just preserve the number).
    double largerThanLong = numeric_limits<long long>::max() * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEvaluates(Value(smallerThanLong), Value(smallerThanLong));
}

TEST_F(ExpressionTruncTest, DecimalArg) {
    assertEvaluates(Value(Decimal128("2")), Value(Decimal128("2.0")));
    assertEvaluates(Value(Decimal128("-2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("0.9")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("0.1")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("-1.2")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("-1.7")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("123456789.9999999999999999999999999")),
                    Value(Decimal128("123456789")));
    assertEvaluates(Value(Decimal128("-99999999999999999999999999999.99")),
                    Value(Decimal128("-99999999999999999999999999999.00")));
    assertEvaluates(Value(Decimal128("3.4E-6000")), Value(Decimal128("0")));
}

TEST_F(ExpressionTruncTest, NullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- Old-style tests -------------------------- */

namespace Add {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
        populateOperands(expression);
        ASSERT_EQUALS(expectedResult(), toBson(expression->evaluate(Document())));
    }

protected:
    virtual void populateOperands(intrusive_ptr<ExpressionNary>& expression) = 0;
    virtual BSONObj expectedResult() = 0;
};

/** $add with a NULL Document pointer, as called by ExpressionNary::optimize().
 */
class NullDocument {
public:
    void run() {
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
        expression->addOperand(ExpressionConstant::create(Value(2)));
        ASSERT_EQUALS(BSON("" << 2), toBson(expression->evaluate(Document())));
    }
};

/** $add without operands. */
class NoOperands : public ExpectedResultBase {
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {}
    virtual BSONObj expectedResult() {
        return BSON("" << 0);
    }
};

/** String type unsupported. */
class String {
public:
    void run() {
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
        expression->addOperand(ExpressionConstant::create(Value("a")));
        ASSERT_THROWS(expression->evaluate(Document()), UserException);
    }
};

/** Bool type unsupported. */
class Bool {
public:
    void run() {
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
        expression->addOperand(ExpressionConstant::create(Value(true)));
        ASSERT_THROWS(expression->evaluate(Document()), UserException);
    }
};

class SingleOperandBase : public ExpectedResultBase {
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {
        expression->addOperand(ExpressionConstant::create(valueFromBson(operand())));
    }
    BSONObj expectedResult() {
        return operand();
    }

protected:
    virtual BSONObj operand() = 0;
};

/** Single int argument. */
class Int : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << 1);
    }
};

/** Single long argument. */
class Long : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << 5555LL);
    }
};

/** Single double argument. */
class Double : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << 99.99);
    }
};

/** Single Date argument. */
class Date : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << Date_t::fromMillisSinceEpoch(12345));
    }
};

/** Single null argument. */
class Null : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << BSONNULL);
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

/** Single undefined argument. */
class Undefined : public SingleOperandBase {
    BSONObj operand() {
        return fromjson("{'':undefined}");
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

class TwoOperandBase : public ExpectedResultBase {
public:
    TwoOperandBase() : _reverse() {}
    void run() {
        ExpectedResultBase::run();
        // Now add the operands in the reverse direction.
        _reverse = true;
        ExpectedResultBase::run();
    }

protected:
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {
        expression->addOperand(
            ExpressionConstant::create(valueFromBson(_reverse ? operand2() : operand1())));
        expression->addOperand(
            ExpressionConstant::create(valueFromBson(_reverse ? operand1() : operand2())));
    }
    virtual BSONObj operand1() = 0;
    virtual BSONObj operand2() = 0;

private:
    bool _reverse;
};

/** Add two ints. */
class IntInt : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 1);
    }
    BSONObj operand2() {
        return BSON("" << 5);
    }
    BSONObj expectedResult() {
        return BSON("" << 6);
    }
};

/** Adding two large ints produces a long, not an overflowed int. */
class IntIntNoOverflow : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << numeric_limits<int>::max());
    }
    BSONObj operand2() {
        return BSON("" << numeric_limits<int>::max());
    }
    BSONObj expectedResult() {
        return BSON("" << ((long long)(numeric_limits<int>::max()) + numeric_limits<int>::max()));
    }
};

/** Adding an int and a long produces a long. */
class IntLong : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 1);
    }
    BSONObj operand2() {
        return BSON("" << 9LL);
    }
    BSONObj expectedResult() {
        return BSON("" << 10LL);
    }
};

/** Adding an int and a long produces a double. */
class IntLongOverflowToDouble : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << numeric_limits<int>::max());
    }
    BSONObj operand2() {
        return BSON("" << numeric_limits<long long>::max());
    }
    BSONObj expectedResult() {
        // When the result cannot be represented in a NumberLong, a NumberDouble is returned.
        const auto im = numeric_limits<int>::max();
        const auto llm = numeric_limits<long long>::max();
        double result = static_cast<double>(im) + static_cast<double>(llm);
        return BSON("" << result);
    }
};

/** Adding an int and a double produces a double. */
class IntDouble : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 9);
    }
    BSONObj operand2() {
        return BSON("" << 1.1);
    }
    BSONObj expectedResult() {
        return BSON("" << 10.1);
    }
};

/** Adding an int and a Date produces a Date. */
class IntDate : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 6);
    }
    BSONObj operand2() {
        return BSON("" << Date_t::fromMillisSinceEpoch(123450));
    }
    BSONObj expectedResult() {
        return BSON("" << Date_t::fromMillisSinceEpoch(123456));
    }
};

/** Adding a long and a double produces a double. */
class LongDouble : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 9LL);
    }
    BSONObj operand2() {
        return BSON("" << 1.1);
    }
    BSONObj expectedResult() {
        return BSON("" << 10.1);
    }
};

/** Adding a long and a double does not overflow. */
class LongDoubleNoOverflow : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << numeric_limits<long long>::max());
    }
    BSONObj operand2() {
        return BSON("" << double(numeric_limits<long long>::max()));
    }
    BSONObj expectedResult() {
        return BSON("" << numeric_limits<long long>::max() +
                        double(numeric_limits<long long>::max()));
    }
};

/** Adding an int and null. */
class IntNull : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 1);
    }
    BSONObj operand2() {
        return BSON("" << BSONNULL);
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

/** Adding a long and undefined. */
class LongUndefined : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 5LL);
    }
    BSONObj operand2() {
        return fromjson("{'':undefined}");
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

}  // namespace Add

namespace And {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        ASSERT_EQUALS(BSON("" << expectedResult()),
                      toBson(expression->evaluate(fromBson(BSON("a" << 1)))));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_EQUALS(BSON("" << expectedResult()),
                      toBson(optimized->evaluate(fromBson(BSON("a" << 1)))));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual bool expectedResult() = 0;
};

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_EQUALS(expectedOptimized(), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class NoOptimizeBase : public OptimizeBase {
    BSONObj expectedOptimized() {
        return constify(spec());
    }
};

/** $and without operands. */
class NoOperands : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSONArray());
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'true'. */
class True : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'false'. */
class False : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'true', 'true'. */
class TrueTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'true', 'false'. */
class TrueFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'false', 'true'. */
class FalseTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(false << true));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'false', 'false'. */
class FalseFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(false << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'true', 'true', 'true'. */
class TrueTrueTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << true << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'true', 'true', 'false'. */
class TrueTrueFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << true << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed '0', '1'. */
class ZeroOne : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(0 << 1));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed '1', '2'. */
class OneTwo : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << 2));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed a field path. */
class FieldPath : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
    bool expectedResult() {
        return true;
    }
};

/** A constant expression is optimized to a constant. */
class OptimizeConstantExpression : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** A non constant expression is not optimized. */
class NonConstant : public NoOptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantTrue : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
    // note: using $and as serialization of ExpressionCoerceToBool rather than
    // ExpressionAnd
};

class ConstantNonConstantFalse : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(0 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with a field path and '1'. */
class NonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a" << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression with a field path and '0'. */
class NonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a" << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with two field paths and '1'. */
class NonConstantNonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"
                                         << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"));
    }
};

/** An expression with two field paths and '0'. */
class NonConstantNonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"
                                         << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with '0', '1', and a field path. */
class ZeroOneNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(0 << 1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with '1', '1', and a field path. */
class OneOneNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << 1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** Nested $and expressions. */
class Nested : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << BSON("$and" << BSON_ARRAY(1)) << "$a"
                                           << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"));
    }
};

/** Nested $and expressions containing a nested value evaluating to false. */
class NestedZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(
                        1 << BSON("$and" << BSON_ARRAY(BSON("$and" << BSON_ARRAY(0)))) << "$a"
                          << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

}  // namespace And

namespace CoerceToBool {

/** Nested expression coerced to true. */
class EvaluateTrue {
public:
    void run() {
        intrusive_ptr<Expression> nested = ExpressionConstant::create(Value(5));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(nested);
        ASSERT(expression->evaluate(Document()).getBool());
    }
};

/** Nested expression coerced to false. */
class EvaluateFalse {
public:
    void run() {
        intrusive_ptr<Expression> nested = ExpressionConstant::create(Value(0));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(nested);
        ASSERT(!expression->evaluate(Document()).getBool());
    }
};

/** Dependencies forwarded from nested expression. */
class Dependencies {
public:
    void run() {
        intrusive_ptr<Expression> nested = ExpressionFieldPath::create("a.b");
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(nested);
        DepsTracker dependencies;
        expression->addDependencies(&dependencies);
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

/** Output to BSONObj. */
class AddToBsonObj {
public:
    void run() {
        intrusive_ptr<Expression> expression =
            ExpressionCoerceToBool::create(ExpressionFieldPath::create("foo"));

        // serialized as $and because CoerceToBool isn't an ExpressionNary
        assertBinaryEqual(fromjson("{field:{$and:['$foo']}}"), toBsonObj(expression));
    }

private:
    static BSONObj toBsonObj(const intrusive_ptr<Expression>& expression) {
        return BSON("field" << expression->serialize(false));
    }
};

/** Output to BSONArray. */
class AddToBsonArray {
public:
    void run() {
        intrusive_ptr<Expression> expression =
            ExpressionCoerceToBool::create(ExpressionFieldPath::create("foo"));

        // serialized as $and because CoerceToBool isn't an ExpressionNary
        assertBinaryEqual(BSON_ARRAY(fromjson("{$and:['$foo']}")), toBsonArray(expression));
    }

private:
    static BSONArray toBsonArray(const intrusive_ptr<Expression>& expression) {
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        return bab.arr();
    }
};

// TODO Test optimize(), difficult because a CoerceToBool cannot be output as
// BSON.

}  // namespace CoerceToBool

namespace Compare {

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_EQUALS(constify(expectedOptimized()), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class FieldRangeOptimize : public OptimizeBase {
    BSONObj expectedOptimized() {
        return spec();
    }
};

class NoOptimize : public OptimizeBase {
    BSONObj expectedOptimized() {
        return spec();
    }
};

/** Check expected result for expressions depending on constants. */
class ExpectedResultBase : public OptimizeBase {
public:
    void run() {
        OptimizeBase::run();
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        // Check expression spec round trip.
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        // Check evaluation result.
        ASSERT_EQUALS(expectedResult(), toBson(expression->evaluate(Document())));
        // Check that the result is the same after optimizing.
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_EQUALS(expectedResult(), toBson(optimized->evaluate(Document())));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedResult() = 0;

private:
    virtual BSONObj expectedOptimized() {
        return BSON("$const" << expectedResult().firstElement());
    }
};

class ExpectedTrue : public ExpectedResultBase {
    BSONObj expectedResult() {
        return BSON("" << true);
    }
};

class ExpectedFalse : public ExpectedResultBase {
    BSONObj expectedResult() {
        return BSON("" << false);
    }
};

class ParseError {
public:
    virtual ~ParseError() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        ASSERT_THROWS(Expression::parseOperand(specElement, vps), UserException);
    }

protected:
    virtual BSONObj spec() = 0;
};

/** $eq with first < second. */
class EqLt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 2));
    }
};

/** $eq with first == second. */
class EqEq : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 1));
    }
};

/** $eq with first > second. */
class EqGt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 0));
    }
};

/** $ne with first < second. */
class NeLt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << 2));
    }
};

/** $ne with first == second. */
class NeEq : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << 1));
    }
};

/** $ne with first > second. */
class NeGt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << 0));
    }
};

/** $gt with first < second. */
class GtLt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(1 << 2));
    }
};

/** $gt with first == second. */
class GtEq : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(1 << 1));
    }
};

/** $gt with first > second. */
class GtGt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(1 << 0));
    }
};

/** $gte with first < second. */
class GteLt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(1 << 2));
    }
};

/** $gte with first == second. */
class GteEq : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(1 << 1));
    }
};

/** $gte with first > second. */
class GteGt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(1 << 0));
    }
};

/** $lt with first < second. */
class LtLt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << 2));
    }
};

/** $lt with first == second. */
class LtEq : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << 1));
    }
};

/** $lt with first > second. */
class LtGt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << 0));
    }
};

/** $lte with first < second. */
class LteLt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(1 << 2));
    }
};

/** $lte with first == second. */
class LteEq : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(1 << 1));
    }
};

/** $lte with first > second. */
class LteGt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(1 << 0));
    }
};

/** $cmp with first < second. */
class CmpLt : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << 2));
    }
    BSONObj expectedResult() {
        return BSON("" << -1);
    }
};

/** $cmp with first == second. */
class CmpEq : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << 1));
    }
    BSONObj expectedResult() {
        return BSON("" << 0);
    }
};

/** $cmp with first > second. */
class CmpGt : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << 0));
    }
    BSONObj expectedResult() {
        return BSON("" << 1);
    }
};

/** $cmp results are bracketed to an absolute value of 1. */
class CmpBracketed : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY("z"
                                         << "a"));
    }
    BSONObj expectedResult() {
        return BSON("" << 1);
    }
};

/** Zero operands provided. */
class ZeroOperands : public ParseError {
    BSONObj spec() {
        return BSON("$ne" << BSONArray());
    }
};

/** One operand provided. */
class OneOperand : public ParseError {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1));
    }
};

/** Three operands provided. */
class ThreeOperands : public ParseError {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(2 << 3 << 4));
    }
};

/** Incompatible types can be compared. */
class IncompatibleTypes {
public:
    void run() {
        BSONObj specObject = BSON("" << BSON("$ne" << BSON_ARRAY("a" << 1)));
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(expression->evaluate(Document()), Value(true));
    }
};

/**
 * An expression depending on constants is optimized to a constant via
 * ExpressionNary::optimize().
 */
class OptimizeConstants : public OptimizeBase {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** $cmp is not optimized. */
class NoOptimizeCmp : public NoOptimize {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << "$a"));
    }
};

/** $ne is not optimized. */
class NoOptimizeNe : public NoOptimize {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << "$a"));
    }
};

/** No optimization is performend without a constant. */
class NoOptimizeNoConstant : public NoOptimize {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY("$a"
                                        << "$b"));
    }
};

/** No optimization is performend without an immediate field path. */
class NoOptimizeWithoutFieldPath : public NoOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(BSON("$and" << BSON_ARRAY("$a")) << 1));
    }
};

/** No optimization is performend without an immediate field path. */
class NoOptimizeWithoutFieldPathReverse : public NoOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << BSON("$and" << BSON_ARRAY("$a"))));
    }
};

/** An equality expression is optimized. */
class OptimizeEq : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY("$a" << 1));
    }
};

/** A reverse sense equality expression is optimized. */
class OptimizeEqReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << "$a"));
    }
};

/** A $lt expression is optimized. */
class OptimizeLt : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY("$a" << 1));
    }
};

/** A reverse sense $lt expression is optimized. */
class OptimizeLtReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << "$a"));
    }
};

/** A $lte expression is optimized. */
class OptimizeLte : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $lte expression is optimized. */
class OptimizeLteReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(2 << "$b"));
    }
};

/** A $gt expression is optimized. */
class OptimizeGt : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $gt expression is optimized. */
class OptimizeGtReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(2 << "$b"));
    }
};

/** A $gte expression is optimized. */
class OptimizeGte : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $gte expression is optimized. */
class OptimizeGteReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(2 << "$b"));
    }
};

}  // namespace Compare

namespace Constant {

/** Create an ExpressionConstant from a Value. */
class Create {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionConstant::create(Value(5));
        assertBinaryEqual(BSON("" << 5), toBson(expression->evaluate(Document())));
    }
};

/** Create an ExpressionConstant from a BsonElement. */
class CreateFromBsonElement {
public:
    void run() {
        BSONObj spec = BSON("IGNORED_FIELD_NAME"
                            << "foo");
        BSONElement specElement = spec.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = ExpressionConstant::parse(specElement, vps);
        assertBinaryEqual(BSON(""
                               << "foo"),
                          toBson(expression->evaluate(Document())));
    }
};

/** No optimization is performed. */
class Optimize {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionConstant::create(Value(5));
        // An attempt to optimize returns the Expression itself.
        ASSERT_EQUALS(expression, expression->optimize());
    }
};

/** No dependencies. */
class Dependencies {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionConstant::create(Value(5));
        DepsTracker dependencies;
        expression->addDependencies(&dependencies);
        ASSERT_EQUALS(0U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

/** Output to BSONObj. */
class AddToBsonObj {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionConstant::create(Value(5));
        // The constant is replaced with a $ expression.
        assertBinaryEqual(BSON("field" << BSON("$const" << 5)), toBsonObj(expression));
    }

private:
    static BSONObj toBsonObj(const intrusive_ptr<Expression>& expression) {
        return BSON("field" << expression->serialize(false));
    }
};

/** Output to BSONArray. */
class AddToBsonArray {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionConstant::create(Value(5));
        // The constant is copied out as is.
        assertBinaryEqual(constify(BSON_ARRAY(5)), toBsonArray(expression));
    }

private:
    static BSONObj toBsonArray(const intrusive_ptr<Expression>& expression) {
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        return bab.obj();
    }
};

}  // namespace Constant

TEST(ExpressionFromAccumulators, Avg) {
    assertExpectedResults("$avg",
                          {// $avg ignores non-numeric inputs.
                           {{Value("string"), Value(BSONNULL), Value(), Value(3)}, Value(3.0)},
                           // $avg always returns a double.
                           {{Value(10LL), Value(20LL)}, Value(15.0)},
                           // $avg returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, Max) {
    assertExpectedResults("$max",
                          {// $max treats non-numeric inputs as valid arguments.
                           {{Value(1), Value(BSONNULL), Value(), Value("a")}, Value("a")},
                           {{Value("a"), Value("b")}, Value("b")},
                           // $max always preserves the type of the result.
                           {{Value(10LL), Value(0.0), Value(5)}, Value(10LL)},
                           // $max returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, Min) {
    assertExpectedResults("$min",
                          {// $min treats non-numeric inputs as valid arguments.
                           {{Value("string")}, Value("string")},
                           {{Value(1), Value(BSONNULL), Value(), Value("a")}, Value(1)},
                           {{Value("a"), Value("b")}, Value("a")},
                           // $min always preserves the type of the result.
                           {{Value(0LL), Value(20.0), Value(10)}, Value(0LL)},
                           // $min returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, Sum) {
    assertExpectedResults(
        "$sum",
        {// $sum ignores non-numeric inputs.
         {{Value("string"), Value(BSONNULL), Value(), Value(3)}, Value(3)},
         // If any argument is a double, $sum returns a double
         {{Value(10LL), Value(10.0)}, Value(20.0)},
         // If no arguments are doubles and an argument is a long, $sum returns a long
         {{Value(10LL), Value(10)}, Value(20LL)},
         // $sum returns 0 when no arguments are provided.
         {{}, Value(0)}});
}

TEST(ExpressionFromAccumulators, StdDevPop) {
    assertExpectedResults("$stdDevPop",
                          {// $stdDevPop ignores non-numeric inputs.
                           {{Value("string"), Value(BSONNULL), Value(), Value(3)}, Value(0.0)},
                           // $stdDevPop always returns a double.
                           {{Value(1LL), Value(3LL)}, Value(1.0)},
                           // $stdDevPop returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, StdDevSamp) {
    assertExpectedResults("$stdDevSamp",
                          {// $stdDevSamp ignores non-numeric inputs.
                           {{Value("string"), Value(BSONNULL), Value(), Value(3)}, Value(BSONNULL)},
                           // $stdDevSamp always returns a double.
                           {{Value(1LL), Value(2LL), Value(3LL)}, Value(1.0)},
                           // $stdDevSamp returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

namespace FieldPath {

/** The provided field path does not pass validation. */
class Invalid {
public:
    void run() {
        ASSERT_THROWS(ExpressionFieldPath::create(""), UserException);
    }
};

/** No optimization is performed. */
class Optimize {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a");
        // An attempt to optimize returns the Expression itself.
        ASSERT_EQUALS(expression, expression->optimize());
    }
};

/** The field path itself is a dependency. */
class Dependencies {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        DepsTracker dependencies;
        expression->addDependencies(&dependencies);
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

/** Field path target field is missing. */
class Missing {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a");
        assertBinaryEqual(fromjson("{}"), toBson(expression->evaluate(Document())));
    }
};

/** Simple case where the target field is present. */
class Present {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a");
        assertBinaryEqual(fromjson("{'':123}"),
                          toBson(expression->evaluate(fromBson(BSON("a" << 123)))));
    }
};

/** Target field parent is null. */
class NestedBelowNull {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:null}")))));
    }
};

/** Target field parent is undefined. */
class NestedBelowUndefined {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:undefined}")))));
    }
};

/** Target field parent is missing. */
class NestedBelowMissing {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(fromjson("{z:1}")))));
    }
};

/** Target field parent is an integer. */
class NestedBelowInt {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{}"), toBson(expression->evaluate(fromBson(BSON("a" << 2)))));
    }
};

/** A value in a nested object. */
class NestedValue {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(BSON("" << 55),
                          toBson(expression->evaluate(fromBson(BSON("a" << BSON("b" << 55))))));
    }
};

/** Target field within an empty object. */
class NestedBelowEmptyObject {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(BSON("a" << BSONObj())))));
    }
};

/** Target field within an empty array. */
class NestedBelowEmptyArray {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(BSON("" << BSONArray()),
                          toBson(expression->evaluate(fromBson(BSON("a" << BSONArray())))));
    }
};

/** Target field within an array containing null. */
class NestedBelowArrayWithNull {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{'':[]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[null]}")))));
    }
};

/** Target field within an array containing undefined. */
class NestedBelowArrayWithUndefined {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{'':[]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[undefined]}")))));
    }
};

/** Target field within an array containing an integer. */
class NestedBelowArrayWithInt {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{'':[]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[1]}")))));
    }
};

/** Target field within an array. */
class NestedWithinArray {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{'':[9]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[{b:9}]}")))));
    }
};

/** Multiple value types within an array. */
class MultipleArrayValues {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b");
        assertBinaryEqual(fromjson("{'':[9,20]}"),
                          toBson(expression->evaluate(
                              fromBson(fromjson("{a:[{b:9},null,undefined,{g:4},{b:20},{}]}")))));
    }
};

/** Expanding values within nested arrays. */
class ExpandNestedArrays {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b.c");
        assertBinaryEqual(fromjson("{'':[[1,2],3,[4],[[5]],[6,7]]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[{b:[{c:1},{c:2}]},"
                                                                        "{b:{c:3}},"
                                                                        "{b:[{c:4}]},"
                                                                        "{b:[{c:[5]}]},"
                                                                        "{b:{c:[6,7]}}]}")))));
    }
};

/** Add to a BSONObj. */
class AddToBsonObj {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b.c");
        assertBinaryEqual(BSON("foo"
                               << "$a.b.c"),
                          BSON("foo" << expression->serialize(false)));
    }
};

/** Add to a BSONArray. */
class AddToBsonArray {
public:
    void run() {
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create("a.b.c");
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        assertBinaryEqual(BSON_ARRAY("$a.b.c"), bab.arr());
    }
};

}  // namespace FieldPath

namespace Object {

class Base {
protected:
    void assertDependencies(const BSONArray& expectedDependencies,
                            const intrusive_ptr<ExpressionObject>& expression,
                            bool includePath = true) const {
        vector<string> path;
        DepsTracker dependencies;
        expression->addDependencies(&dependencies, includePath ? &path : 0);
        BSONArrayBuilder bab;
        for (set<string>::const_iterator i = dependencies.fields.begin();
             i != dependencies.fields.end();
             ++i) {
            bab << *i;
        }
        ASSERT_EQUALS(expectedDependencies, bab.arr());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.needTextScore);
    }
};

class ExpectedResultBase : public Base {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        _expression = ExpressionObject::createRoot();
        prepareExpression();
        Document document = fromBson(source());
        MutableDocument result;
        Variables vars(0, document);
        expression()->addToDocument(result, document, &vars);
        assertBinaryEqual(expected(), toBson(result.freeze()));
        assertDependencies(expectedDependencies(), _expression);
        ASSERT_EQUALS(expectedBsonRepresentation(), expressionToBson(_expression));
        ASSERT_EQUALS(expectedIsSimple(), _expression->isSimple());
    }

protected:
    intrusive_ptr<ExpressionObject> expression() {
        return _expression;
    }
    virtual BSONObj source() {
        return BSON("_id" << 0 << "a" << 1 << "b" << 2);
    }
    virtual void prepareExpression() = 0;
    virtual BSONObj expected() = 0;
    virtual BSONArray expectedDependencies() = 0;
    virtual BSONObj expectedBsonRepresentation() = 0;
    virtual bool expectedIsSimple() {
        return true;
    }

private:
    intrusive_ptr<ExpressionObject> _expression;
};

/** Empty object spec. */
class Empty : public ExpectedResultBase {
public:
    void prepareExpression() {}
    BSONObj expected() {
        return BSON("_id" << 0);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return BSONObj();
    }
};

/** Include 'a' field only. */
class Include : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a");
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << 1);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << true);
    }
};

/** Cannot include missing 'a' field. */
class MissingInclude : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0 << "b" << 2);
    }
    void prepareExpression() {
        expression()->includePath("a");
    }
    BSONObj expected() {
        return BSON("_id" << 0);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << true);
    }
};

/** Include '_id' field only. */
class IncludeId : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("_id");
    }
    BSONObj expected() {
        return BSON("_id" << 0);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("_id" << true);
    }
};

/** Exclude '_id' field. */
class ExcludeId : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("b");
        expression()->excludeId(true);
    }
    BSONObj expected() {
        return BSON("b" << 2);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("_id" << false << "b" << true);
    }
};

/** Result order based on source document field order, not inclusion spec field
 * order. */
class SourceOrder : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("b");
        expression()->includePath("a");
    }
    BSONObj expected() {
        return source();
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a"
                          << "b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("b" << true << "a" << true);
    }
};

/** Include a nested field. */
class IncludeNested : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a.b");
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5));
    }
    BSONObj source() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5 << "c" << 6) << "z" << 2);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true));
    }
};

/** Include two nested fields. */
class IncludeTwoNested : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a.b");
        expression()->includePath("a.c");
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5 << "c" << 6));
    }
    BSONObj source() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5 << "c" << 6) << "z" << 2);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b"
                          << "a.c");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true << "c" << true));
    }
};

/** Include two fields nested within different parents. */
class IncludeTwoParentNested : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a.b");
        expression()->includePath("c.d");
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5) << "c" << BSON("d" << 6));
    }
    BSONObj source() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5) << "c" << BSON("d" << 6) << "z" << 2);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b"
                          << "c.d");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true) << "c" << BSON("d" << true));
    }
};

/** Attempt to include a missing nested field. */
class IncludeMissingNested : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a.b");
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSONObj());
    }
    BSONObj source() {
        return BSON("_id" << 0 << "a" << BSON("c" << 6) << "z" << 2);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true));
    }
};

/** Attempt to include a nested field within a non object. */
class IncludeNestedWithinNonObject : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a.b");
    }
    BSONObj expected() {
        return BSON("_id" << 0);
    }
    BSONObj source() {
        return BSON("_id" << 0 << "a" << 2 << "z" << 2);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true));
    }
};

/** Include a nested field within an array. */
class IncludeArrayNested : public ExpectedResultBase {
public:
    void prepareExpression() {
        expression()->includePath("a.b");
    }
    BSONObj expected() {
        return fromjson("{_id:0,a:[{b:5},{b:2},{}]}");
    }
    BSONObj source() {
        return fromjson("{_id:0,a:[{b:5,c:6},{b:2,c:9},{c:7},[],2],z:1}");
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true));
    }
};

/** Don't include not root '_id' field implicitly. */
class ExcludeNonRootId : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0 << "a" << BSON("_id" << 1 << "b" << 1));
    }
    void prepareExpression() {
        expression()->includePath("a.b");
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 1));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << true));
    }
};

/** Project a computed expression. */
class Computed : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << 5);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("$const" << 5));
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** Project a computed expression replacing an existing field. */
class ComputedReplacement : public Computed {
    virtual BSONObj source() {
        return BSON("_id" << 0 << "a" << 99);
    }
};

/** An undefined value is passed through */
class ComputedUndefined : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a"),
                               ExpressionConstant::create(Value(BSONUndefined)));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSONUndefined);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return fromjson("{a:{$const:undefined}}");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** Project a computed expression replacing an existing field with Undefined. */
class ComputedUndefinedReplacement : public ComputedUndefined {
    virtual BSONObj source() {
        return BSON("_id" << 0 << "a" << 99);
    }
};

/** A null value is projected. */
class ComputedNull : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(BSONNULL)));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSONNULL);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("$const" << BSONNULL));
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** A nested value is projected. */
class ComputedNested : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(5)));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 5));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b" << BSON("$const" << 5)));
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** A field path is projected. */
class ComputedFieldPath : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0 << "x" << 4);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a"), ExpressionFieldPath::create("x"));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << 4);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "x");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a"
                    << "$x");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** A nested field path is projected. */
class ComputedNestedFieldPath : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0 << "x" << BSON("y" << 4));
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a.b"), ExpressionFieldPath::create("x.y"));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 4));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "x.y");
    }
    BSONObj expectedBsonRepresentation() {
        return BSON("a" << BSON("b"
                                << "$x.y"));
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** An empty subobject expression for a missing field is not projected. */
class EmptyNewSubobject : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        // Create a sub expression returning an empty object.
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->addField(mongo::FieldPath("b"), ExpressionFieldPath::create("a.b"));
        expression()->addField(mongo::FieldPath("a"), subExpression);
    }
    BSONObj expected() {
        return BSON("_id" << 0);
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id"
                          << "a.b");
    }
    BSONObj expectedBsonRepresentation() {
        return fromjson("{a:{b:'$a.b'}}");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** A non empty subobject expression for a missing field is projected. */
class NonEmptyNewSubobject : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        // Create a sub expression returning an empty object.
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->addField(mongo::FieldPath("b"), ExpressionConstant::create(Value(6)));
        expression()->addField(mongo::FieldPath("a"), subExpression);
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 6));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return fromjson("{a:{b:{$const:6}}}");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** Two computed fields within a common parent. */
class AdjacentDottedComputedFields : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(6)));
        expression()->addField(mongo::FieldPath("a.c"), ExpressionConstant::create(Value(7)));
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 6 << "c" << 7));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return fromjson("{a:{b:{$const:6},c:{$const:7}}}");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** Two computed fields within a common parent, in one case dotted. */
class AdjacentDottedAndNestedComputedFields : public AdjacentDottedComputedFields {
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(6)));
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->addField(mongo::FieldPath("c"), ExpressionConstant::create(Value(7)));
        expression()->addField(mongo::FieldPath("a"), subExpression);
    }
};

/** Two computed fields within a common parent, in another case dotted. */
class AdjacentNestedAndDottedComputedFields : public AdjacentDottedComputedFields {
    void prepareExpression() {
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->addField(mongo::FieldPath("b"), ExpressionConstant::create(Value(6)));
        expression()->addField(mongo::FieldPath("a"), subExpression);
        expression()->addField(mongo::FieldPath("a.c"), ExpressionConstant::create(Value(7)));
    }
};

/** Two computed fields within a common parent, nested rather than dotted. */
class AdjacentNestedComputedFields : public AdjacentDottedComputedFields {
    void prepareExpression() {
        intrusive_ptr<ExpressionObject> firstSubExpression = ExpressionObject::create();
        firstSubExpression->addField(mongo::FieldPath("b"), ExpressionConstant::create(Value(6)));
        expression()->addField(mongo::FieldPath("a"), firstSubExpression);
        intrusive_ptr<ExpressionObject> secondSubExpression = ExpressionObject::create();
        secondSubExpression->addField(mongo::FieldPath("c"), ExpressionConstant::create(Value(7)));
        expression()->addField(mongo::FieldPath("a"), secondSubExpression);
    }
};

/** Field ordering is preserved when nested fields are merged. */
class AdjacentNestedOrdering : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(6)));
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        // Add field 'd' then 'c'.  Expect the same field ordering in the result
        // doc.
        subExpression->addField(mongo::FieldPath("d"), ExpressionConstant::create(Value(7)));
        subExpression->addField(mongo::FieldPath("c"), ExpressionConstant::create(Value(8)));
        expression()->addField(mongo::FieldPath("a"), subExpression);
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << 6 << "d" << 7 << "c" << 8));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return fromjson("{a:{b:{$const:6},d:{$const:7},c:{$const:8}}}");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** Adjacent fields two levels deep. */
class MultipleNestedFields : public ExpectedResultBase {
public:
    virtual BSONObj source() {
        return BSON("_id" << 0);
    }
    void prepareExpression() {
        expression()->addField(mongo::FieldPath("a.b.c"), ExpressionConstant::create(Value(6)));
        intrusive_ptr<ExpressionObject> bSubExpression = ExpressionObject::create();
        bSubExpression->addField(mongo::FieldPath("d"), ExpressionConstant::create(Value(7)));
        intrusive_ptr<ExpressionObject> aSubExpression = ExpressionObject::create();
        aSubExpression->addField(mongo::FieldPath("b"), bSubExpression);
        expression()->addField(mongo::FieldPath("a"), aSubExpression);
    }
    BSONObj expected() {
        return BSON("_id" << 0 << "a" << BSON("b" << BSON("c" << 6 << "d" << 7)));
    }
    BSONArray expectedDependencies() {
        return BSON_ARRAY("_id");
    }
    BSONObj expectedBsonRepresentation() {
        return fromjson("{a:{b:{c:{$const:6},d:{$const:7}}}}");
    }
    bool expectedIsSimple() {
        return false;
    }
};

/** Two expressions cannot generate the same field. */
class ConflictingExpressionFields : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        ASSERT_THROWS(expression->addField(mongo::FieldPath("a"),  // Duplicate field.
                                           ExpressionConstant::create(Value(6))),
                      UserException);
    }
};

/** An expression field conflicts with an inclusion field. */
class ConflictingInclusionExpressionFields : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->includePath("a");
        ASSERT_THROWS(
            expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(6))),
            UserException);
    }
};

/** An inclusion field conflicts with an expression field. */
class ConflictingExpressionInclusionFields : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        ASSERT_THROWS(expression->includePath("a"), UserException);
    }
};

/** An object expression conflicts with a constant expression. */
class ConflictingObjectConstantExpressionFields : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->includePath("b");
        expression->addField(mongo::FieldPath("a"), subExpression);
        ASSERT_THROWS(
            expression->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(6))),
            UserException);
    }
};

/** A constant expression conflicts with an object expression. */
class ConflictingConstantObjectExpressionFields : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(6)));
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->includePath("b");
        ASSERT_THROWS(expression->addField(mongo::FieldPath("a"), subExpression), UserException);
    }
};

/** Two nested expressions cannot generate the same field. */
class ConflictingNestedFields : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(5)));
        ASSERT_THROWS(expression->addField(mongo::FieldPath("a.b"),  // Duplicate field.
                                           ExpressionConstant::create(Value(6))),
                      UserException);
    }
};

/** An expression cannot be created for a subfield of another expression. */
class ConflictingFieldAndSubfield : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        ASSERT_THROWS(
            expression->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(5))),
            UserException);
    }
};

/** An expression cannot be created for a nested field of another expression. */
class ConflictingFieldAndNestedField : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->addField(mongo::FieldPath("b"), ExpressionConstant::create(Value(5)));
        ASSERT_THROWS(expression->addField(mongo::FieldPath("a"), subExpression), UserException);
    }
};

/** An expression cannot be created for a parent field of another expression. */
class ConflictingSubfieldAndField : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a.b"), ExpressionConstant::create(Value(5)));
        ASSERT_THROWS(
            expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5))),
            UserException);
    }
};

/** An expression cannot be created for a parent of a nested field. */
class ConflictingNestedFieldAndField : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
        subExpression->addField(mongo::FieldPath("b"), ExpressionConstant::create(Value(5)));
        expression->addField(mongo::FieldPath("a"), subExpression);
        ASSERT_THROWS(
            expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5))),
            UserException);
    }
};

/** Dependencies for non inclusion expressions. */
class NonInclusionDependencies : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        assertDependencies(BSON_ARRAY("_id"), expression, true);
        assertDependencies(BSONArray(), expression, false);
        expression->addField(mongo::FieldPath("b"), ExpressionFieldPath::create("c.d"));
        assertDependencies(BSON_ARRAY("_id"
                                      << "c.d"),
                           expression,
                           true);
        assertDependencies(BSON_ARRAY("c.d"), expression, false);
    }
};

/** Dependencies for inclusion expressions. */
class InclusionDependencies : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->includePath("a");
        assertDependencies(BSON_ARRAY("_id"
                                      << "a"),
                           expression,
                           true);
        DepsTracker unused;
        // 'path' must be provided for inclusion expressions.
        ASSERT_THROWS(expression->addDependencies(&unused), UserException);
    }
};

/** Optimizing an object expression optimizes its sub expressions. */
class Optimize : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        // Add inclusion.
        expression->includePath("a");
        // Add non inclusion.
        intrusive_ptr<Expression> andExpr = new ExpressionAnd();
        expression->addField(mongo::FieldPath("b"), andExpr);
        expression->optimize();
        // Optimizing 'expression' optimizes its non inclusion sub expressions,
        // while
        // inclusion sub expressions are passed through.
        ASSERT_EQUALS(BSON("a" << true << "b" << BSON("$const" << true)),
                      expressionToBson(expression));
    }
};

/** Serialize to a BSONObj. */
class AddToBsonObj : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        ASSERT_EQUALS(constify(BSON("foo" << BSON("a" << 5))),
                      BSON("foo" << expression->serialize(false)));
    }
};

/** Serialize to a BSONObj, with constants represented by expressions. */
class AddToBsonObjRequireExpression : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        ASSERT_EQUALS(BSON("foo" << BSON("a" << BSON("$const" << 5))),
                      BSON("foo" << expression->serialize(false)));
    }
};

/** Serialize to a BSONArray. */
class AddToBsonArray : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->addField(mongo::FieldPath("a"), ExpressionConstant::create(Value(5)));
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        ASSERT_EQUALS(constify(BSON_ARRAY(BSON("a" << 5))), bab.arr());
    }
};

/**
 * evaluate() does not supply an inclusion document.  Inclusion spec'd fields
 * are not
 * included.  (Inclusion specs are not generally expected/allowed in cases where
 * evaluate
 * is called instead of addToDocument.)
 */
class Evaluate : public Base {
public:
    void run() {
        intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
        expression->includePath("a");
        expression->addField(mongo::FieldPath("b"), ExpressionConstant::create(Value(5)));
        expression->addField(mongo::FieldPath("c"), ExpressionFieldPath::create("a"));
        ASSERT_EQUALS(
            BSON("b" << 5 << "c" << 1),
            toBson(expression->evaluate(fromBson(BSON("_id" << 0 << "a" << 1))).getDocument()));
    }
};

}  // namespace Object

namespace Or {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        ASSERT_EQUALS(BSON("" << expectedResult()),
                      toBson(expression->evaluate(fromBson(BSON("a" << 1)))));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_EQUALS(BSON("" << expectedResult()),
                      toBson(optimized->evaluate(fromBson(BSON("a" << 1)))));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual bool expectedResult() = 0;
};

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_EQUALS(expectedOptimized(), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class NoOptimizeBase : public OptimizeBase {
    BSONObj expectedOptimized() {
        return constify(spec());
    }
};

/** $or without operands. */
class NoOperands : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSONArray());
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'true'. */
class True : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'false'. */
class False : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'true', 'true'. */
class TrueTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(true << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'true', 'false'. */
class TrueFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(true << false));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'false', 'true'. */
class FalseTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'false', 'false'. */
class FalseFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'false', 'false', 'false'. */
class FalseFalseFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << false << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'false', 'false', 'true'. */
class FalseFalseTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << false << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed '0', '1'. */
class ZeroOne : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << 1));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed '0', 'false'. */
class ZeroFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed a field path. */
class FieldPath : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
    bool expectedResult() {
        return true;
    }
};

/** A constant expression is optimized to a constant. */
class OptimizeConstantExpression : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** A non constant expression is not optimized. */
class NonConstant : public NoOptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantTrue : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantFalse : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
    // note: using $and as serialization of ExpressionCoerceToBool rather than
    // ExpressionAnd
};

/** An expression with a field path and '1'. */
class NonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a" << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression with a field path and '0'. */
class NonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a" << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression with two field paths and '1'. */
class NonConstantNonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"
                                        << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression with two field paths and '0'. */
class NonConstantNonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"
                                        << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"));
    }
};

/** An expression with '0', '1', and a field path. */
class ZeroOneNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << 1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression with '0', '0', and a field path. */
class ZeroZeroNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << 0 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** Nested $or expressions. */
class Nested : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << BSON("$or" << BSON_ARRAY(0)) << "$a"
                                          << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"));
    }
};

/** Nested $or expressions containing a nested value evaluating to false. */
class NestedOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << BSON("$or" << BSON_ARRAY(BSON("$or" << BSON_ARRAY(1))))
                                          << "$a"
                                          << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

}  // namespace Or

namespace Parse {

namespace Object {

class Base {
public:
    virtual ~Base() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        Expression::ObjectCtx context = objectCtx();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression =
            Expression::parseObject(specElement.Obj(), &context, vps);
        ASSERT_EQUALS(expectedBson(), expressionToBson(expression));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual Expression::ObjectCtx objectCtx() {
        return Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK);
    }
    virtual BSONObj expectedBson() {
        return constify(spec());
    }
};

class ParseError {
public:
    virtual ~ParseError() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        Expression::ObjectCtx context = objectCtx();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        ASSERT_THROWS(Expression::parseObject(specElement.Obj(), &context, vps), UserException);
    }

protected:
    virtual BSONObj spec() = 0;
    virtual Expression::ObjectCtx objectCtx() {
        return Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK);
    }
};

/** The spec must be an object. */
class NonObject {
public:
    void run() {
        BSONObj specObject = BSON("" << 1);
        BSONElement specElement = specObject.firstElement();
        Expression::ObjectCtx context = Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK);
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        ASSERT_THROWS(Expression::parseObject(specElement.Obj(), &context, vps), UserException);
    }
};

/** Empty object. */
class Empty : public Base {
    BSONObj spec() {
        return BSONObj();
    }
};

/** Operator spec object. */
class Operator : public Base {
    BSONObj spec() {
        return BSON("$and" << BSONArray());
    }
};

/** Invalid operator not allowed. */
class InvalidOperator : public ParseError {
    BSONObj spec() {
        return BSON("$invalid" << 1);
    }
};

/** Two operators not allowed. */
class TwoOperators : public ParseError {
    BSONObj spec() {
        return BSON("$and" << BSONArray() << "$or" << BSONArray());
    }
};

/** An operator must be the first and only field. */
class OperatorLaterField : public ParseError {
    BSONObj spec() {
        return BSON("a" << BSON("$and" << BSONArray()) << "$or" << BSONArray());
    }
};

/** An operator must be the first and only field. */
class OperatorAndOtherField : public ParseError {
    BSONObj spec() {
        return BSON("$and" << BSONArray() << "a" << BSON("$or" << BSONArray()));
    }
};

/** Operators not allowed at the top level of a projection. */
class OperatorTopLevel : public ParseError {
    BSONObj spec() {
        return BSON("$and" << BSONArray());
    }
    Expression::ObjectCtx objectCtx() {
        return Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK |
                                     Expression::ObjectCtx::TOP_LEVEL);
    }
};

/** Dotted fields are not generally allowed. */
class Dotted : public ParseError {
    BSONObj spec() {
        return BSON("a.b" << BSON("$and" << BSONArray()));
    }
};

/** Dotted fields are allowed at the top level. */
class DottedTopLevel : public Base {
    BSONObj spec() {
        return BSON("a.b" << BSON("$and" << BSONArray()));
    }
    Expression::ObjectCtx objectCtx() {
        return Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK |
                                     Expression::ObjectCtx::TOP_LEVEL);
    }
    BSONObj expectedBson() {
        return BSON("a" << BSON("b" << BSON("$and" << BSONArray())));
    }
};

/** Nested spec. */
class Nested : public Base {
    BSONObj spec() {
        return BSON("a" << BSON("$and" << BSONArray()));
    }
};

/** Parse error in nested document. */
class NestedParseError : public ParseError {
    BSONObj spec() {
        return BSON("a" << BSON("$and" << BSONArray() << "$or" << BSONArray()));
    }
};

/** FieldPath expression. */
class FieldPath : public Base {
    BSONObj spec() {
        return BSON("a"
                    << "$field");
    }
};

/** Invalid FieldPath expression. */
class InvalidFieldPath : public ParseError {
    BSONObj spec() {
        return BSON("a"
                    << "$field.");
    }
};

/** Non FieldPath string. */
class NonFieldPathString : public ParseError {
    BSONObj spec() {
        return BSON("a"
                    << "foo");
    }
};

/** Inclusion spec not allowed. */
class DisallowedInclusion : public ParseError {
    BSONObj spec() {
        return BSON("a" << 1);
    }
};

class InclusionBase : public Base {
    Expression::ObjectCtx objectCtx() {
        return Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK |
                                     Expression::ObjectCtx::INCLUSION_OK);
    }
    BSONObj expectedBson() {
        return BSON("a" << true);
    }
};

/** Inclusion with bool type. */
class InclusionBool : public InclusionBase {
    BSONObj spec() {
        return BSON("a" << true);
    }
};

/** Inclusion with double type. */
class InclusionDouble : public InclusionBase {
    BSONObj spec() {
        return BSON("a" << 1.0);
    }
};

/** Inclusion with int type. */
class InclusionInt : public InclusionBase {
    BSONObj spec() {
        return BSON("a" << 1);
    }
};

/** Inclusion with long type. */
class InclusionLong : public InclusionBase {
    BSONObj spec() {
        return BSON("a" << 1LL);
    }
};

/** Inclusion of a nested field. */
class NestedInclusion : public InclusionBase {
    BSONObj spec() {
        return BSON("a" << BSON("b" << true));
    }
    BSONObj expectedBson() {
        return spec();
    }
};

/** Exclude _id. */
class ExcludeId : public Base {
    BSONObj spec() {
        return BSON("_id" << 0);
    }
    Expression::ObjectCtx objectCtx() {
        return Expression::ObjectCtx(Expression::ObjectCtx::DOCUMENT_OK |
                                     Expression::ObjectCtx::TOP_LEVEL);
    }
    BSONObj expectedBson() {
        return BSON("_id" << false);
    }
};

/** Excluding non _id field not allowed. */
class ExcludeNonId : public ParseError {
    BSONObj spec() {
        return BSON("a" << 0);
    }
};

/** Excluding _id not top level. */
class ExcludeIdNotTopLevel : public ParseError {
    BSONObj spec() {
        return BSON("_id" << 0);
    }
};

/** Invalid value type. */
class InvalidType : public ParseError {
    BSONObj spec() {
        return BSON("a" << BSONNULL);
    }
};

}  // namespace Object

namespace Expression {

using mongo::Expression;

class Base {
public:
    virtual ~Base() {}
    void run() {
        BSONObj specObject = spec();
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseExpression(specElement, vps);
        ASSERT_EQUALS(constify(expectedBson()), expressionToBson(expression));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedBson() {
        return constify(spec());
    }
};

class ParseError {
public:
    virtual ~ParseError() {}
    void run() {
        BSONObj specObject = spec();
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        ASSERT_THROWS(Expression::parseExpression(specElement, vps), UserException);
    }

protected:
    virtual BSONObj spec() = 0;
};

/** A constant expression. */
class Const : public Base {
    BSONObj spec() {
        return BSON("$const" << 5);
    }
};

/** An expression with an invalid name. */
class InvalidName : public ParseError {
    BSONObj spec() {
        return BSON("$invalid" << 1);
    }
};

/** An expression requiring an array that is not provided with an array. */
class RequiredArrayMissing : public ParseError {
    BSONObj spec() {
        return BSON("$strcasecmp"
                    << "foo");
    }
};

/** An expression with the wrong number of operands. */
class IncorrectOperandCount : public ParseError {
    BSONObj spec() {
        return BSON("$strcasecmp" << BSON_ARRAY("foo"));
    }
};

/** An expression with the correct number of operands. */
class CorrectOperandCount : public Base {
    BSONObj spec() {
        return BSON("$strcasecmp" << BSON_ARRAY("foo"
                                                << "FOO"));
    }
};

/** An variable argument expression with zero operands. */
class ZeroOperands : public Base {
    BSONObj spec() {
        return BSON("$and" << BSONArray());
    }
};

/** An variable argument expression with one operand. */
class OneOperand : public Base {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1));
    }
};

/** An variable argument expression with two operands. */
class TwoOperands : public Base {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << 2));
    }
};

/** An variable argument expression with a singleton operand. */
class SingletonOperandVariable : public Base {
    BSONObj spec() {
        return BSON("$and" << 1);
    }
    BSONObj expectedBson() {
        return BSON("$and" << BSON_ARRAY(1));
    }
};

/** An fixed argument expression with a singleton operand. */
class SingletonOperandFixed : public Base {
    BSONObj spec() {
        return BSON("$not" << 1);
    }
    BSONObj expectedBson() {
        return BSON("$not" << BSON_ARRAY(1));
    }
};

/** An object can be provided as a singleton argument. */
class ObjectSingleton : public Base {
    BSONObj spec() {
        return BSON("$and" << BSON("$const" << 1));
    }
    BSONObj expectedBson() {
        return BSON("$and" << BSON_ARRAY(BSON("$const" << 1)));
    }
};

/** An object can be provided as an array agrument. */
class ObjectOperand : public Base {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(BSON("$const" << 1)));
    }
    BSONObj expectedBson() {
        return BSON("$and" << BSON_ARRAY(1));
    }
};

}  // namespace Expression

namespace Operand {

class Base {
public:
    virtual ~Base() {}
    void run() {
        BSONObj specObject = spec();
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<mongo::Expression> expression =
            mongo::Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(expectedBson(), expressionToBson(expression));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedBson() {
        return constify(spec());
    }
};

class ParseError {
public:
    virtual ~ParseError() {}
    void run() {
        BSONObj specObject = spec();
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        ASSERT_THROWS(mongo::Expression::parseOperand(specElement, vps), UserException);
    }

protected:
    virtual BSONObj spec() = 0;
};

/** A field path operand. */
class FieldPath {
public:
    void run() {
        BSONObj specObject = BSON(""
                                  << "$field");
        BSONElement specElement = specObject.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<mongo::Expression> expression =
            mongo::Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(specObject, BSON("" << expression->serialize(false)));
    }
};

/** A string constant (not field path) operand. */
class NonFieldPathString : public Base {
    BSONObj spec() {
        return BSON(""
                    << "foo");
    }
    BSONObj expectedBson() {
        return BSON("$const"
                    << "foo");
    }
};

/** An object operand. */
class Object : public Base {
    BSONObj spec() {
        return BSON("" << BSON("$and" << BSONArray()));
    }
    BSONObj expectedBson() {
        return BSON("$and" << BSONArray());
    }
};

/** An inclusion operand. */
class InclusionObject : public ParseError {
    BSONObj spec() {
        return BSON("" << BSON("a" << 1));
    }
};

/** A constant operand. */
class Constant : public Base {
    BSONObj spec() {
        return BSON("" << 5);
    }
    BSONObj expectedBson() {
        return BSON("$const" << 5);
    }
};

}  // namespace Operand

}  // namespace Parse

namespace Set {
Value sortSet(Value set) {
    if (set.nullish()) {
        return Value(BSONNULL);
    }
    vector<Value> sortedSet = set.getArray();
    sort(sortedSet.begin(), sortedSet.end());
    return Value(sortedSet);
}

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        const Document spec = getSpec();
        const Value args = spec["input"];
        if (!spec["expected"].missing()) {
            FieldIterator fields(spec["expected"].getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value expected = field.second;
                const BSONObj obj = BSON(field.first << args);
                VariablesIdGenerator idGenerator;
                VariablesParseState vps(&idGenerator);
                const intrusive_ptr<Expression> expr =
                    Expression::parseExpression(obj.firstElement(), vps);
                Value result = expr->evaluate(Document());
                if (result.getType() == Array) {
                    result = sortSet(result);
                }
                if (result != expected) {
                    string errMsg = str::stream()
                        << "for expression " << field.first.toString() << " with argument "
                        << args.toString() << " full tree: " << expr->serialize(false).toString()
                        << " expected: " << expected.toString()
                        << " but got: " << result.toString();
                    FAIL(errMsg);
                }
                // TODO test optimize here
            }
        }
        if (!spec["error"].missing()) {
            const vector<Value>& asserters = spec["error"].getArray();
            size_t n = asserters.size();
            for (size_t i = 0; i < n; i++) {
                const BSONObj obj = BSON(asserters[i].getString() << args);
                VariablesIdGenerator idGenerator;
                VariablesParseState vps(&idGenerator);
                ASSERT_THROWS(
                    {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(obj.firstElement(), vps);
                        expr->evaluate(Document());
                    },
                    UserException);
            }
        }
    }

private:
    virtual Document getSpec() = 0;
};

class Same : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class Redundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class DoubleRedundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(DOC_ARRAY(1 << 1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2)
                                          << "$setUnion"
                                          << DOC_ARRAY(1 << 2)
                                          << "$setDifference"
                                          << vector<Value>()));
    }
};

class Super : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(2)));
    }
};

class SuperWithRedundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 2) << DOC_ARRAY(1)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(2)));
    }
};

class Sub : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class SameBackwards : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(2 << 1)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class NoOverlap : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 4)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << vector<Value>()
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2 << 4 << 8)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(1 << 2)));
    }
};

class Overlap : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 2 << 4)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2 << 4 << 8)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(1)));
    }
};

class LastNull : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << Value(BSONNULL)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference"
                                                     << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"
                                        << "$setIsSubset"));
    }
};

class FirstNull : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(Value(BSONNULL) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference"
                                                     << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"
                                        << "$setIsSubset"));
    }
};

class NoArg : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << vector<Value>() << "expected"
                    << DOC("$setIntersection" << vector<Value>() << "$setUnion" << vector<Value>())
                    << "error"
                    << DOC_ARRAY("$setEquals"
                                 << "$setIsSubset"
                                 << "$setDifference"));
    }
};

class OneArg : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << DOC_ARRAY(1 << 2) << "$setUnion"
                                                     << DOC_ARRAY(1 << 2))
                           << "error"
                           << DOC_ARRAY("$setEquals"
                                        << "$setIsSubset"
                                        << "$setDifference"));
    }
};

class EmptyArg : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(vector<Value>()) << "expected"
                    << DOC("$setIntersection" << vector<Value>() << "$setUnion" << vector<Value>())
                    << "error"
                    << DOC_ARRAY("$setEquals"
                                 << "$setIsSubset"
                                 << "$setDifference"));
    }
};

class LeftArgEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(vector<Value>() << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << vector<Value>() << "$setUnion"
                                                     << DOC_ARRAY(1 << 2)
                                                     << "$setIsSubset"
                                                     << true
                                                     << "$setEquals"
                                                     << false
                                                     << "$setDifference"
                                                     << vector<Value>()));
    }
};

class RightArgEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << vector<Value>()) << "expected"
                           << DOC("$setIntersection" << vector<Value>() << "$setUnion"
                                                     << DOC_ARRAY(1 << 2)
                                                     << "$setIsSubset"
                                                     << false
                                                     << "$setEquals"
                                                     << false
                                                     << "$setDifference"
                                                     << DOC_ARRAY(1 << 2)));
    }
};

class ManyArgs : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(DOC_ARRAY(8 << 3) << DOC_ARRAY("asdf"
                                                                << "foo")
                                                   << DOC_ARRAY(80.3 << 34)
                                                   << vector<Value>()
                                                   << DOC_ARRAY(80.3 << "foo" << 11 << "yay"))
                    << "expected"
                    << DOC("$setIntersection" << vector<Value>() << "$setEquals" << false
                                              << "$setUnion"
                                              << DOC_ARRAY(3 << 8 << 11 << 34 << 80.3 << "asdf"
                                                             << "foo"
                                                             << "yay"))
                    << "error"
                    << DOC_ARRAY("$setIsSubset"
                                 << "$setDifference"));
    }
};

class ManyArgsEqual : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 4) << DOC_ARRAY(1 << 2 << 2 << 4)
                                                               << DOC_ARRAY(4 << 1 << 2)
                                                               << DOC_ARRAY(2 << 1 << 1 << 4))
                           << "expected"
                           << DOC("$setIntersection" << DOC_ARRAY(1 << 2 << 4) << "$setEquals"
                                                     << true
                                                     << "$setUnion"
                                                     << DOC_ARRAY(1 << 2 << 4))
                           << "error"
                           << DOC_ARRAY("$setIsSubset"
                                        << "$setDifference"));
    }
};
}  // namespace Set

namespace Strcasecmp {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        assertResult(expectedResult(), spec());
        assertResult(-expectedResult(), reverseSpec());
    }

protected:
    virtual string a() = 0;
    virtual string b() = 0;
    virtual int expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$strcasecmp" << BSON_ARRAY(a() << b()));
    }
    BSONObj reverseSpec() {
        return BSON("$strcasecmp" << BSON_ARRAY(b() << a()));
    }
    void assertResult(int expectedResult, const BSONObj& spec) {
        BSONObj specObj = BSON("" << spec);
        BSONElement specElement = specObj.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec), expressionToBson(expression));
        ASSERT_EQUALS(BSON("" << expectedResult), toBson(expression->evaluate(Document())));
    }
};

class NullBegin : public ExpectedResultBase {
    string a() {
        return string("\0ab", 3);
    }
    string b() {
        return string("\0AB", 3);
    }
    int expectedResult() {
        return 0;
    }
};

class NullEnd : public ExpectedResultBase {
    string a() {
        return string("ab\0", 3);
    }
    string b() {
        return string("aB\0", 3);
    }
    int expectedResult() {
        return 0;
    }
};

class NullMiddleLt : public ExpectedResultBase {
    string a() {
        return string("a\0a", 3);
    }
    string b() {
        return string("a\0B", 3);
    }
    int expectedResult() {
        return -1;
    }
};

class NullMiddleEq : public ExpectedResultBase {
    string a() {
        return string("a\0b", 3);
    }
    string b() {
        return string("a\0B", 3);
    }
    int expectedResult() {
        return 0;
    }
};

class NullMiddleGt : public ExpectedResultBase {
    string a() {
        return string("a\0c", 3);
    }
    string b() {
        return string("a\0B", 3);
    }
    int expectedResult() {
        return 1;
    }
};

}  // namespace Strcasecmp

namespace StrLenBytes {

TEST(ExpressionStrLenBytes, ComputesLengthOfString) {
    assertExpectedResults("$strLenBytes", {{{Value("abc")}, Value(3)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenBytes", {{{Value("")}, Value(0)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenBytes", {{{Value("ab\0c"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenBytes", {{{Value("abc\0"_sd)}, Value(4)}});
}

}  // namespace StrLenBytes

namespace StrLenCP {

TEST(ExpressionStrLenCP, ComputesLengthOfASCIIString) {
    assertExpectedResults("$strLenCP", {{{Value("abc")}, Value(3)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenCP", {{{Value("")}, Value(0)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenCP", {{{Value("ab\0c"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenCP", {{{Value("abc\0"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithAccent) {
    assertExpectedResults("$strLenCP", {{{Value("a\0bâ"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithSpecialCharacters) {
    assertExpectedResults("$strLenCP", {{{Value("ºabøåß")}, Value(6)}});
}

}  // namespace StrLenCP

namespace SubstrBytes {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        ASSERT_EQUALS(BSON("" << expectedResult()), toBson(expression->evaluate(Document())));
    }

protected:
    virtual string str() = 0;
    virtual int offset() = 0;
    virtual int length() = 0;
    virtual string expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$substrBytes" << BSON_ARRAY(str() << offset() << length()));
    }
};

/** Retrieve a full string containing a null character. */
class FullNull : public ExpectedResultBase {
    string str() {
        return string("a\0b", 3);
    }
    int offset() {
        return 0;
    }
    int length() {
        return 3;
    }
    string expectedResult() {
        return str();
    }
};

/** Retrieve a substring beginning with a null character. */
class BeginAtNull : public ExpectedResultBase {
    string str() {
        return string("a\0b", 3);
    }
    int offset() {
        return 1;
    }
    int length() {
        return 2;
    }
    string expectedResult() {
        return string("\0b", 2);
    }
};

/** Retrieve a substring ending with a null character. */
class EndAtNull : public ExpectedResultBase {
    string str() {
        return string("a\0b", 3);
    }
    int offset() {
        return 0;
    }
    int length() {
        return 2;
    }
    string expectedResult() {
        return string("a\0", 2);
    }
};

/** Drop a beginning null character. */
class DropBeginningNull : public ExpectedResultBase {
    string str() {
        return string("\0b", 2);
    }
    int offset() {
        return 1;
    }
    int length() {
        return 1;
    }
    string expectedResult() {
        return "b";
    }
};

/** Drop an ending null character. */
class DropEndingNull : public ExpectedResultBase {
    string str() {
        return string("a\0", 2);
    }
    int offset() {
        return 0;
    }
    int length() {
        return 1;
    }
    string expectedResult() {
        return "a";
    }
};

}  // namespace Substr

namespace SubstrCP {

TEST(ExpressionSubstrCPTest, DoesThrowWithBadContinuationByte) {
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);

    const auto continuationByte = "\x80\x00"_sd;
    const auto expr = Expression::parseExpression(
        BSON("$substrCP" << BSON_ARRAY(continuationByte << 0 << 1)).firstElement(), vps);
    ASSERT_THROWS({ expr->evaluate(Document()); }, UserException);
}

TEST(ExpressionSubstrCPTest, DoesThrowWithInvalidLeadingByte) {
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);

    const auto leadingByte = "\xFF\x00"_sd;
    const auto expr = Expression::parseExpression(
        BSON("$substrCP" << BSON_ARRAY(leadingByte << 0 << 1)).firstElement(), vps);
    ASSERT_THROWS({ expr->evaluate(Document()); }, UserException);
}

TEST(ExpressionSubstrCPTest, WithStandardValue) {
    assertExpectedResults("$substrCP", {{{Value("abc"), Value(0), Value(2)}, Value("ab")}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacter) {
    assertExpectedResults("$substrCP", {{{Value("abc\0d"), Value(2), Value(3)}, Value("c\0d")}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacterAtEnd) {
    assertExpectedResults("$substrCP", {{{Value("abc\0"), Value(2), Value(2)}, Value("c\0")}});
}

TEST(ExpressionSubstrCPTest, WithOutOfRangeString) {
    assertExpectedResults("$substrCP", {{{Value("abc"), Value(3), Value(2)}, Value("")}});
}

TEST(ExpressionSubstrCPTest, WithPartiallyOutOfRangeString) {
    assertExpectedResults("$substrCP", {{{Value("abc"), Value(1), Value(4)}, Value("bc")}});
}

TEST(ExpressionSubstrCPTest, WithUnicodeValue) {
    assertExpectedResults("$substrCP", {{{Value("øø∫å"), Value(0), Value(4)}, Value("øø∫å")}});
    assertExpectedResults("$substrBytes", {{{Value("øø∫å"), Value(0), Value(4)}, Value("øø")}});
}

TEST(ExpressionSubstrCPTest, WithMixedUnicodeAndASCIIValue) {
    assertExpectedResults("$substrCP", {{{Value("a∫bøßabc"), Value(1), Value(4)}, Value("∫bøß")}});
    assertExpectedResults("$substrBytes", {{{Value("a∫bøßabc"), Value(1), Value(4)}, Value("∫b")}});
}

}  // namespace SubstrCP

namespace Type {

TEST(ExpressionTypeTest, WithMinKeyValue) {
    assertExpectedResults("$type", {{{Value(MINKEY)}, Value("minKey")}});
}

TEST(ExpressionTypeTest, WithDoubleValue) {
    assertExpectedResults("$type", {{{Value(1.0)}, Value("double")}});
}

TEST(ExpressionTypeTest, WithStringValue) {
    assertExpectedResults("$type", {{{Value("stringValue")}, Value("string")}});
}

TEST(ExpressionTypeTest, WithObjectValue) {
    BSONObj objectVal = fromjson("{a: {$literal: 1}}");
    assertExpectedResults("$type", {{{Value(objectVal)}, Value("object")}});
}

TEST(ExpressionTypeTest, WithArrayValue) {
    assertExpectedResults("$type", {{{Value(BSON_ARRAY(1 << 2))}, Value("array")}});
}

TEST(ExpressionTypeTest, WithBinDataValue) {
    BSONBinData binDataVal = BSONBinData("", 0, BinDataGeneral);
    assertExpectedResults("$type", {{{Value(binDataVal)}, Value("binData")}});
}

TEST(ExpressionTypeTest, WithUndefinedValue) {
    assertExpectedResults("$type", {{{Value(BSONUndefined)}, Value("undefined")}});
}

TEST(ExpressionTypeTest, WithOIDValue) {
    assertExpectedResults("$type", {{{Value(OID())}, Value("objectId")}});
}

TEST(ExpressionTypeTest, WithBoolValue) {
    assertExpectedResults("$type", {{{Value(true)}, Value("bool")}});
}

TEST(ExpressionTypeTest, WithDateValue) {
    Date_t dateVal = BSON("" << DATENOW).firstElement().Date();
    assertExpectedResults("$type", {{{Value(dateVal)}, Value("date")}});
}

TEST(ExpressionTypeTest, WithNullValue) {
    assertExpectedResults("$type", {{{Value(BSONNULL)}, Value("null")}});
}

TEST(ExpressionTypeTest, WithRegexValue) {
    assertExpectedResults("$type", {{{Value(BSONRegEx("a.b"))}, Value("regex")}});
}

TEST(ExpressionTypeTest, WithSymbolValue) {
    assertExpectedResults("$type", {{{Value(BSONSymbol("a"))}, Value("symbol")}});
}

TEST(ExpressionTypeTest, WithDBRefValue) {
    assertExpectedResults("$type", {{{Value(BSONDBRef("", OID()))}, Value("dbPointer")}});
}

TEST(ExpressionTypeTest, WithCodeWScopeValue) {
    assertExpectedResults(
        "$type", {{{Value(BSONCodeWScope("var x = 3", BSONObj()))}, Value("javascriptWithScope")}});
}

TEST(ExpressionTypeTest, WithCodeValue) {
    assertExpectedResults("$type", {{{Value(BSONCode("var x = 3"))}, Value("javascript")}});
}

TEST(ExpressionTypeTest, WithIntValue) {
    assertExpectedResults("$type", {{{Value(1)}, Value("int")}});
}

TEST(ExpressionTypeTest, WithDecimalValue) {
    assertExpectedResults("$type", {{{Value(Decimal128(0.3))}, Value("decimal")}});
}

TEST(ExpressionTypeTest, WithLongValue) {
    assertExpectedResults("$type", {{{Value(1LL)}, Value("long")}});
}

TEST(ExpressionTypeTest, WithTimestampValue) {
    assertExpectedResults("$type", {{{Value(Timestamp(0, 0))}, Value("timestamp")}});
}

TEST(ExpressionTypeTest, WithMaxKeyValue) {
    assertExpectedResults("$type", {{{Value(MAXKEY)}, Value("maxKey")}});
}

}  // namespace Type

namespace ToLower {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        ASSERT_EQUALS(BSON("" << expectedResult()), toBson(expression->evaluate(Document())));
    }

protected:
    virtual string str() = 0;
    virtual string expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$toLower" << BSON_ARRAY(str()));
    }
};

/** String beginning with a null character. */
class NullBegin : public ExpectedResultBase {
    string str() {
        return string("\0aB", 3);
    }
    string expectedResult() {
        return string("\0ab", 3);
    }
};

/** String containing a null character. */
class NullMiddle : public ExpectedResultBase {
    string str() {
        return string("a\0B", 3);
    }
    string expectedResult() {
        return string("a\0b", 3);
    }
};

/** String ending with a null character. */
class NullEnd : public ExpectedResultBase {
    string str() {
        return string("aB\0", 3);
    }
    string expectedResult() {
        return string("ab\0", 3);
    }
};

}  // namespace ToLower

namespace ToUpper {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> expression = Expression::parseOperand(specElement, vps);
        ASSERT_EQUALS(constify(spec()), expressionToBson(expression));
        ASSERT_EQUALS(BSON("" << expectedResult()), toBson(expression->evaluate(Document())));
    }

protected:
    virtual string str() = 0;
    virtual string expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$toUpper" << BSON_ARRAY(str()));
    }
};

/** String beginning with a null character. */
class NullBegin : public ExpectedResultBase {
    string str() {
        return string("\0aB", 3);
    }
    string expectedResult() {
        return string("\0AB", 3);
    }
};

/** String containing a null character. */
class NullMiddle : public ExpectedResultBase {
    string str() {
        return string("a\0B", 3);
    }
    string expectedResult() {
        return string("A\0B", 3);
    }
};

/** String ending with a null character. */
class NullEnd : public ExpectedResultBase {
    string str() {
        return string("aB\0", 3);
    }
    string expectedResult() {
        return string("AB\0", 3);
    }
};

}  // namespace ToUpper

namespace AllAnyElements {
class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        const Document spec = getSpec();
        const Value args = spec["input"];
        if (!spec["expected"].missing()) {
            FieldIterator fields(spec["expected"].getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value expected = field.second;
                const BSONObj obj = BSON(field.first << args);
                VariablesIdGenerator idGenerator;
                VariablesParseState vps(&idGenerator);
                const intrusive_ptr<Expression> expr =
                    Expression::parseExpression(obj.firstElement(), vps);
                const Value result = expr->evaluate(Document());
                if (result != expected) {
                    string errMsg = str::stream()
                        << "for expression " << field.first.toString() << " with argument "
                        << args.toString() << " full tree: " << expr->serialize(false).toString()
                        << " expected: " << expected.toString()
                        << " but got: " << result.toString();
                    FAIL(errMsg);
                }
                // TODO test optimize here
            }
        }
        if (!spec["error"].missing()) {
            const vector<Value>& asserters = spec["error"].getArray();
            size_t n = asserters.size();
            for (size_t i = 0; i < n; i++) {
                const BSONObj obj = BSON(asserters[i].getString() << args);
                VariablesIdGenerator idGenerator;
                VariablesParseState vps(&idGenerator);
                ASSERT_THROWS(
                    {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(obj.firstElement(), vps);
                        expr->evaluate(Document());
                    },
                    UserException);
            }
        }
    }

private:
    virtual Document getSpec() = 0;
};

class JustFalse : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(false)) << "expected"
                           << DOC("$allElementsTrue" << false << "$anyElementTrue" << false));
    }
};

class JustTrue : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(true)) << "expected"
                           << DOC("$allElementsTrue" << true << "$anyElementTrue" << true));
    }
};

class OneTrueOneFalse : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(true << false)) << "expected"
                           << DOC("$allElementsTrue" << false << "$anyElementTrue" << true));
    }
};

class Empty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(vector<Value>()) << "expected"
                           << DOC("$allElementsTrue" << true << "$anyElementTrue" << false));
    }
};

class TrueViaInt : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1)) << "expected"
                           << DOC("$allElementsTrue" << true << "$anyElementTrue" << true));
    }
};

class FalseViaInt : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(0)) << "expected"
                           << DOC("$allElementsTrue" << false << "$anyElementTrue" << false));
    }
};

class Null : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(BSONNULL) << "error" << DOC_ARRAY("$allElementsTrue"
                                                                          << "$anyElementTrue"));
    }
};

}  // namespace AllAnyElements

class All : public Suite {
public:
    All() : Suite("expression") {}
    void setupTests() {
        add<Add::NullDocument>();
        add<Add::NoOperands>();
        add<Add::Date>();
        add<Add::String>();
        add<Add::Bool>();
        add<Add::Int>();
        add<Add::Long>();
        add<Add::Double>();
        add<Add::Null>();
        add<Add::Undefined>();
        add<Add::IntInt>();
        add<Add::IntIntNoOverflow>();
        add<Add::IntLong>();
        add<Add::IntLongOverflowToDouble>();
        add<Add::IntDouble>();
        add<Add::IntDate>();
        add<Add::LongDouble>();
        add<Add::LongDoubleNoOverflow>();
        add<Add::IntNull>();
        add<Add::LongUndefined>();

        add<And::NoOperands>();
        add<And::True>();
        add<And::False>();
        add<And::TrueTrue>();
        add<And::TrueFalse>();
        add<And::FalseTrue>();
        add<And::FalseFalse>();
        add<And::TrueTrueTrue>();
        add<And::TrueTrueFalse>();
        add<And::TrueTrueFalse>();
        add<And::ZeroOne>();
        add<And::OneTwo>();
        add<And::FieldPath>();
        add<And::OptimizeConstantExpression>();
        add<And::NonConstant>();
        add<And::ConstantNonConstantTrue>();
        add<And::ConstantNonConstantFalse>();
        add<And::NonConstantOne>();
        add<And::NonConstantZero>();
        add<And::NonConstantNonConstantOne>();
        add<And::NonConstantNonConstantZero>();
        add<And::ZeroOneNonConstant>();
        add<And::OneOneNonConstant>();
        add<And::Nested>();
        add<And::NestedZero>();

        add<CoerceToBool::EvaluateTrue>();
        add<CoerceToBool::EvaluateFalse>();
        add<CoerceToBool::Dependencies>();
        add<CoerceToBool::AddToBsonObj>();
        add<CoerceToBool::AddToBsonArray>();

        add<Compare::EqLt>();
        add<Compare::EqEq>();
        add<Compare::EqGt>();
        add<Compare::NeLt>();
        add<Compare::NeEq>();
        add<Compare::NeGt>();
        add<Compare::GtLt>();
        add<Compare::GtEq>();
        add<Compare::GtGt>();
        add<Compare::GteLt>();
        add<Compare::GteEq>();
        add<Compare::GteGt>();
        add<Compare::LtLt>();
        add<Compare::LtEq>();
        add<Compare::LtGt>();
        add<Compare::LteLt>();
        add<Compare::LteEq>();
        add<Compare::LteGt>();
        add<Compare::CmpLt>();
        add<Compare::CmpEq>();
        add<Compare::CmpGt>();
        add<Compare::CmpBracketed>();
        add<Compare::ZeroOperands>();
        add<Compare::OneOperand>();
        add<Compare::ThreeOperands>();
        add<Compare::IncompatibleTypes>();
        add<Compare::OptimizeConstants>();
        add<Compare::NoOptimizeCmp>();
        add<Compare::NoOptimizeNe>();
        add<Compare::NoOptimizeNoConstant>();
        add<Compare::NoOptimizeWithoutFieldPath>();
        add<Compare::NoOptimizeWithoutFieldPathReverse>();
        add<Compare::OptimizeEq>();
        add<Compare::OptimizeEqReverse>();
        add<Compare::OptimizeLt>();
        add<Compare::OptimizeLtReverse>();
        add<Compare::OptimizeLte>();
        add<Compare::OptimizeLteReverse>();
        add<Compare::OptimizeGt>();
        add<Compare::OptimizeGtReverse>();
        add<Compare::OptimizeGte>();
        add<Compare::OptimizeGteReverse>();

        add<Constant::Create>();
        add<Constant::CreateFromBsonElement>();
        add<Constant::Optimize>();
        add<Constant::Dependencies>();
        add<Constant::AddToBsonObj>();
        add<Constant::AddToBsonArray>();

        add<FieldPath::Invalid>();
        add<FieldPath::Optimize>();
        add<FieldPath::Dependencies>();
        add<FieldPath::Missing>();
        add<FieldPath::Present>();
        add<FieldPath::NestedBelowNull>();
        add<FieldPath::NestedBelowUndefined>();
        add<FieldPath::NestedBelowMissing>();
        add<FieldPath::NestedBelowInt>();
        add<FieldPath::NestedValue>();
        add<FieldPath::NestedBelowEmptyObject>();
        add<FieldPath::NestedBelowEmptyArray>();
        add<FieldPath::NestedBelowEmptyArray>();
        add<FieldPath::NestedBelowArrayWithNull>();
        add<FieldPath::NestedBelowArrayWithUndefined>();
        add<FieldPath::NestedBelowArrayWithInt>();
        add<FieldPath::NestedWithinArray>();
        add<FieldPath::MultipleArrayValues>();
        add<FieldPath::ExpandNestedArrays>();
        add<FieldPath::AddToBsonObj>();
        add<FieldPath::AddToBsonArray>();

        add<Object::Empty>();
        add<Object::Include>();
        add<Object::MissingInclude>();
        add<Object::IncludeId>();
        add<Object::ExcludeId>();
        add<Object::SourceOrder>();
        add<Object::IncludeNested>();
        add<Object::IncludeTwoNested>();
        add<Object::IncludeTwoParentNested>();
        add<Object::IncludeMissingNested>();
        add<Object::IncludeNestedWithinNonObject>();
        add<Object::IncludeArrayNested>();
        add<Object::ExcludeNonRootId>();
        add<Object::Computed>();
        add<Object::ComputedReplacement>();
        add<Object::ComputedUndefined>();
        add<Object::ComputedUndefinedReplacement>();
        add<Object::ComputedNull>();
        add<Object::ComputedNested>();
        add<Object::ComputedFieldPath>();
        add<Object::ComputedNestedFieldPath>();
        add<Object::EmptyNewSubobject>();
        add<Object::NonEmptyNewSubobject>();
        add<Object::AdjacentNestedComputedFields>();
        add<Object::AdjacentDottedAndNestedComputedFields>();
        add<Object::AdjacentNestedAndDottedComputedFields>();
        add<Object::AdjacentDottedComputedFields>();
        add<Object::AdjacentNestedOrdering>();
        add<Object::MultipleNestedFields>();
        add<Object::ConflictingExpressionFields>();
        add<Object::ConflictingInclusionExpressionFields>();
        add<Object::ConflictingExpressionInclusionFields>();
        add<Object::ConflictingObjectConstantExpressionFields>();
        add<Object::ConflictingConstantObjectExpressionFields>();
        add<Object::ConflictingNestedFields>();
        add<Object::ConflictingFieldAndSubfield>();
        add<Object::ConflictingFieldAndNestedField>();
        add<Object::ConflictingSubfieldAndField>();
        add<Object::ConflictingNestedFieldAndField>();
        add<Object::NonInclusionDependencies>();
        add<Object::InclusionDependencies>();
        add<Object::Optimize>();
        add<Object::AddToBsonObj>();
        add<Object::AddToBsonObjRequireExpression>();
        add<Object::AddToBsonArray>();
        add<Object::Evaluate>();

        add<Or::NoOperands>();
        add<Or::True>();
        add<Or::False>();
        add<Or::TrueTrue>();
        add<Or::TrueFalse>();
        add<Or::FalseTrue>();
        add<Or::FalseFalse>();
        add<Or::FalseFalseFalse>();
        add<Or::FalseFalseTrue>();
        add<Or::ZeroOne>();
        add<Or::ZeroFalse>();
        add<Or::FieldPath>();
        add<Or::OptimizeConstantExpression>();
        add<Or::NonConstant>();
        add<Or::ConstantNonConstantTrue>();
        add<Or::ConstantNonConstantFalse>();
        add<Or::NonConstantOne>();
        add<Or::NonConstantZero>();
        add<Or::NonConstantNonConstantOne>();
        add<Or::NonConstantNonConstantZero>();
        add<Or::ZeroOneNonConstant>();
        add<Or::ZeroZeroNonConstant>();
        add<Or::Nested>();
        add<Or::NestedOne>();

        add<Parse::Object::NonObject>();
        add<Parse::Object::Empty>();
        add<Parse::Object::Operator>();
        add<Parse::Object::InvalidOperator>();
        add<Parse::Object::TwoOperators>();
        add<Parse::Object::OperatorLaterField>();
        add<Parse::Object::OperatorAndOtherField>();
        add<Parse::Object::OperatorTopLevel>();
        add<Parse::Object::Dotted>();
        add<Parse::Object::DottedTopLevel>();
        add<Parse::Object::Nested>();
        add<Parse::Object::NestedParseError>();
        add<Parse::Object::FieldPath>();
        add<Parse::Object::InvalidFieldPath>();
        add<Parse::Object::NonFieldPathString>();
        add<Parse::Object::DisallowedInclusion>();
        add<Parse::Object::InclusionBool>();
        add<Parse::Object::InclusionDouble>();
        add<Parse::Object::InclusionInt>();
        add<Parse::Object::InclusionLong>();
        add<Parse::Object::NestedInclusion>();
        add<Parse::Object::ExcludeId>();
        add<Parse::Object::ExcludeNonId>();
        add<Parse::Object::ExcludeIdNotTopLevel>();
        add<Parse::Object::InvalidType>();
        add<Parse::Expression::Const>();
        add<Parse::Expression::InvalidName>();
        add<Parse::Expression::RequiredArrayMissing>();
        add<Parse::Expression::IncorrectOperandCount>();
        add<Parse::Expression::CorrectOperandCount>();
        add<Parse::Expression::ZeroOperands>();
        add<Parse::Expression::OneOperand>();
        add<Parse::Expression::TwoOperands>();
        add<Parse::Expression::SingletonOperandVariable>();
        add<Parse::Expression::SingletonOperandFixed>();
        add<Parse::Expression::ObjectSingleton>();
        add<Parse::Expression::ObjectOperand>();
        add<Parse::Operand::FieldPath>();
        add<Parse::Operand::NonFieldPathString>();
        add<Parse::Operand::Object>();
        add<Parse::Operand::InclusionObject>();
        add<Parse::Operand::Constant>();

        add<Strcasecmp::NullBegin>();
        add<Strcasecmp::NullEnd>();
        add<Strcasecmp::NullMiddleLt>();
        add<Strcasecmp::NullMiddleEq>();
        add<Strcasecmp::NullMiddleGt>();

        add<SubstrBytes::FullNull>();
        add<SubstrBytes::BeginAtNull>();
        add<SubstrBytes::EndAtNull>();
        add<SubstrBytes::DropBeginningNull>();
        add<SubstrBytes::DropEndingNull>();

        add<ToLower::NullBegin>();
        add<ToLower::NullMiddle>();
        add<ToLower::NullEnd>();

        add<ToUpper::NullBegin>();
        add<ToUpper::NullMiddle>();
        add<ToUpper::NullEnd>();

        add<Set::Same>();
        add<Set::Redundant>();
        add<Set::DoubleRedundant>();
        add<Set::Sub>();
        add<Set::Super>();
        add<Set::SameBackwards>();
        add<Set::NoOverlap>();
        add<Set::Overlap>();
        add<Set::FirstNull>();
        add<Set::LastNull>();
        add<Set::NoArg>();
        add<Set::OneArg>();
        add<Set::EmptyArg>();
        add<Set::LeftArgEmpty>();
        add<Set::RightArgEmpty>();
        add<Set::ManyArgs>();
        add<Set::ManyArgsEqual>();

        add<AllAnyElements::JustFalse>();
        add<AllAnyElements::JustTrue>();
        add<AllAnyElements::OneTrueOneFalse>();
        add<AllAnyElements::Empty>();
        add<AllAnyElements::TrueViaInt>();
        add<AllAnyElements::FalseViaInt>();
        add<AllAnyElements::Null>();
    }
};

SuiteInstance<All> myall;

}  // namespace ExpressionTests
