/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript model conversion
 *
 * The parser is barescriptParser.bare, an include library script that runs on this runtime and
 * produces the JSON "BareScript model" - the same model the JavaScript and Python implementations
 * use. This file converts that model into the runtime's compiled representation and back.
 *
 * The compiled representation is what the evaluator walks: statements and expressions as C structs
 * rather than objects, with two things resolved up front that the model leaves by name - a jump's
 * label becomes a statement index, and a function-local variable becomes a slot index.
 */

#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"
#include "barescript/parser.h"
#include "barescript/runtime.h"

#include "internal.h"


/* The binary and unary operator text, indexed by operator */
const char *bsBinaryOpText[BS_BINARY_COUNT] = {
    "**", "*", "/", "%", "+", "-", "<<", ">>", "<=", "<", ">=", ">", "==", "!=", "&", "^", "|", "&&", "||"
};

const char *bsUnaryOpText[BS_UNARY_COUNT] = {"-", "!", "~"};


/*
 * Freeing the compiled representation
 */


void bsExprFree(BSExpr *expr)
{
    if (expr == NULL) {
        return;
    }
    switch (expr->type) {
    case BS_EXPR_STRING:
        bsRelease(expr->u.string);
        break;
    case BS_EXPR_VARIABLE:
        bsRelease(expr->u.variable.name);
        break;
    case BS_EXPR_FUNCTION:
    case BS_EXPR_CALL0:
    case BS_EXPR_CALL1:
    case BS_EXPR_CALL2:
        for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
            bsExprFree(expr->u.function.args[ix]);
        }
        bsRelease(expr->u.function.name);
        free(expr->u.function.args);
        break;
    case BS_EXPR_BINARY:
        bsExprFree(expr->u.binary.left);
        bsExprFree(expr->u.binary.right);
        break;
    case BS_EXPR_UNARY:
        bsExprFree(expr->u.unary.expr);
        break;
    case BS_EXPR_GROUP:
        bsExprFree(expr->u.group);
        break;
    default:
        break;
    }
    free(expr);
}


static void bsStatementFree(BSStatement *statement)
{
    switch (statement->type) {
    case BS_STMT_EXPR:
        bsRelease(statement->u.expr.name);
        bsExprFree(statement->u.expr.expr);
        break;
    case BS_STMT_JUMP:
        bsRelease(statement->u.jump.label);
        bsExprFree(statement->u.jump.expr);
        break;
    case BS_STMT_RETURN:
        bsExprFree(statement->u.ret.expr);
        break;
    case BS_STMT_LABEL:
        bsRelease(statement->u.label.name);
        break;
    case BS_STMT_FUNCTION:
        break;
    default:
        for (size_t ix = 0; ix < statement->u.include.count; ix++) {
            bsRelease(statement->u.include.includes[ix].url);
        }
        free(statement->u.include.includes);
        break;
    }
    free(statement);
}


void bsStatementsFree(BSStatement **statements, size_t statementCount)
{
    for (size_t ix = 0; ix < statementCount; ix++) {
        bsStatementFree(statements[ix]);
    }
    free(statements);
}


void bsFunctionDefFree(BSFunctionDef *def)
{
    bsRelease(def->name);
    for (size_t ix = 0; ix < def->argCount; ix++) {
        bsRelease(def->argNames[ix]);
    }
    free(def->argNames);
    for (size_t ix = 0; ix < def->slotCount; ix++) {
        bsRelease(def->slotNames[ix]);
    }
    free(def->slotNames);
    bsStatementsFree(def->statements, def->statementCount);
    free(def);
}


BSScript *bsScriptRetain(BSScript *script)
{
    script->refcount++;
    return script;
}


void bsScriptRelease(BSScript *script)
{
    if (script == NULL || --script->refcount != 0) {
        return;
    }
    bsStatementsFree(script->statements, script->statementCount);
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        bsFunctionDefFree(script->functions[ix]);
    }
    free(script->functions);
    bsRelease(script->scriptName);
    bsRelease(script->scriptLines);
    free(script->coverageCounts);
    free(script);
}


/*
 * Model to compiled representation
 */


static BSExpr *bsExprNew(BSExprType type)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = type;
    return expr;
}

/* Number, string, and variable expressions cannot reassign a name. A call can. */
static void bsExprFinish(BSExpr *expr)
{
    switch (expr->type) {
    case BS_EXPR_NUMBER:
    case BS_EXPR_STRING:
    case BS_EXPR_VARIABLE:
        expr->pure = 1;
        break;
    case BS_EXPR_FUNCTION: {
        size_t argCount = expr->u.function.argCount;
        unsigned char later = 0;
        bool seen = false;
        for (size_t ix = argCount; ix-- > 0; ) {
            if (seen && ix < 8) {
                later |= (unsigned char) (1u << ix);
            }
            if (!expr->u.function.args[ix]->pure) {
                seen = true;
            }
        }
        expr->laterEffectful = later;
        if (!expr->u.function.isIf && argCount <= 2) {
            expr->type = (BSExprType) (BS_EXPR_CALL0 + argCount);
        }
        break;
    }
    case BS_EXPR_BINARY:
        expr->pure = (unsigned char) (expr->u.binary.left->pure & expr->u.binary.right->pure);
        break;
    case BS_EXPR_UNARY:
        expr->pure = expr->u.unary.expr->pure;
        break;
    default:
        expr->pure = expr->u.group->pure;
        break;
    }
}

static BSValue bsInternName(BSValue name)
{
    return bsStringIntern(bsStringData(name), bsStringSize(name));
}


BSExpr *bsExprFromModel(BSValue model)
{
    if (model.type != BS_OBJECT) {
        return NULL;
    }

    BSValue number = bsObjectGet(model, "number");
    if (number.type == BS_NUMBER) {
        BSExpr *expr = bsExprNew(BS_EXPR_NUMBER);
        expr->u.number = number.u.number;
        bsExprFinish(expr);
        return expr;
    }

    BSValue string = bsObjectGet(model, "string");
    if (string.type == BS_STRING) {
        BSExpr *expr = bsExprNew(BS_EXPR_STRING);
        expr->u.string = bsStringIntern(bsStringData(string), bsStringSize(string));
        bsExprFinish(expr);
        return expr;
    }

    BSValue variable = bsObjectGet(model, "variable");
    if (variable.type == BS_STRING) {
        BSExpr *expr = bsExprNew(BS_EXPR_VARIABLE);
        expr->u.variable.name = bsInternName(variable);
        expr->u.variable.slot = -1;
        const char *name = bsStringData(variable);
        if (strcmp(name, "null") == 0) {
            expr->u.variable.special = BS_SPECIAL_NULL;
        } else if (strcmp(name, "true") == 0) {
            expr->u.variable.special = BS_SPECIAL_TRUE;
        } else if (strcmp(name, "false") == 0) {
            expr->u.variable.special = BS_SPECIAL_FALSE;
        }
        bsExprFinish(expr);
        return expr;
    }

    BSValue function = bsObjectGet(model, "function");
    if (function.type == BS_OBJECT) {
        BSValue name = bsObjectGet(function, "name");
        if (name.type != BS_STRING) {
            return NULL;
        }
        BSValue args = bsObjectGet(function, "args");
        BSExpr *expr = bsExprNew(BS_EXPR_FUNCTION);
        expr->u.function.name = bsInternName(name);
        expr->u.function.slot = -1;
        expr->u.function.isIf = (strcmp(bsStringData(name), "if") == 0);
        size_t argCount = bsArrayCount(args);
        if (argCount != 0) {
            expr->u.function.args = bsAlloc(argCount * sizeof(BSExpr *));
            for (size_t ix = 0; ix < argCount; ix++) {
                BSExpr *arg = bsExprFromModel(bsArrayGet(args, ix));
                if (arg == NULL) {
                    bsExprFree(expr);
                    return NULL;
                }
                expr->u.function.args[expr->u.function.argCount++] = arg;
            }
        }
        bsExprFinish(expr);
        return expr;
    }

    BSValue binary = bsObjectGet(model, "binary");
    if (binary.type == BS_OBJECT) {
        BSValue op = bsObjectGet(binary, "op");
        if (op.type != BS_STRING) {
            return NULL;
        }
        int opIndex = -1;
        for (int ix = 0; ix < BS_BINARY_COUNT; ix++) {
            if (strcmp(bsBinaryOpText[ix], bsStringData(op)) == 0) {
                opIndex = ix;
                break;
            }
        }
        if (opIndex < 0) {
            return NULL;
        }
        BSExpr *left = bsExprFromModel(bsObjectGet(binary, "left"));
        BSExpr *right = bsExprFromModel(bsObjectGet(binary, "right"));
        if (left == NULL || right == NULL) {
            bsExprFree(left);
            bsExprFree(right);
            return NULL;
        }
        BSExpr *expr = bsExprNew(BS_EXPR_BINARY);
        expr->u.binary.op = (BSBinaryOp) opIndex;
        expr->u.binary.left = left;
        expr->u.binary.right = right;
        bsExprFinish(expr);
        return expr;
    }

    BSValue unary = bsObjectGet(model, "unary");
    if (unary.type == BS_OBJECT) {
        BSValue op = bsObjectGet(unary, "op");
        if (op.type != BS_STRING) {
            return NULL;
        }
        int opIndex = -1;
        for (int ix = 0; ix < BS_UNARY_COUNT; ix++) {
            if (strcmp(bsUnaryOpText[ix], bsStringData(op)) == 0) {
                opIndex = ix;
                break;
            }
        }
        if (opIndex < 0) {
            return NULL;
        }
        BSExpr *operand = bsExprFromModel(bsObjectGet(unary, "expr"));
        if (operand == NULL) {
            return NULL;
        }
        BSExpr *expr = bsExprNew(BS_EXPR_UNARY);
        expr->u.unary.op = (BSUnaryOp) opIndex;
        expr->u.unary.expr = operand;
        bsExprFinish(expr);
        return expr;
    }

    if (bsObjectHas(model, "group")) {
        BSExpr *inner = bsExprFromModel(bsObjectGet(model, "group"));
        if (inner == NULL) {
            return NULL;
        }
        BSExpr *expr = bsExprNew(BS_EXPR_GROUP);
        expr->u.group = inner;
        bsExprFinish(expr);
        return expr;
    }

    return NULL;
}


/* A growable statement list */
typedef struct BSStatementList {
    BSStatement **statements;
    size_t count;
    size_t capacity;
} BSStatementList;


static BSStatement *bsStatementAdd(BSStatementList *list, BSStatementType type, BSValue base)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity != 0 ? list->capacity * 2 : 16;
        list->statements = bsRealloc(list->statements, list->capacity * sizeof(BSStatement *));
    }
    BSStatement *statement = bsAlloc(sizeof(BSStatement));
    memset(statement, 0, sizeof(BSStatement));
    statement->type = type;

    BSValue lineNumber = bsObjectGet(base, "lineNumber");
    BSValue lineCount = bsObjectGet(base, "lineCount");
    statement->lineNumber = lineNumber.type == BS_NUMBER ? (int) lineNumber.u.number : 0;
    statement->lineCount = lineCount.type == BS_NUMBER ? (int) lineCount.u.number : 0;

    if (type == BS_STMT_EXPR) {
        statement->u.expr.name = bsNull();
        statement->u.expr.slot = -1;
    } else if (type == BS_STMT_JUMP) {
        statement->u.jump.label = bsNull();
        statement->u.jump.index = -1;
    } else if (type == BS_STMT_LABEL) {
        statement->u.label.name = bsNull();
    }
    list->statements[list->count++] = statement;
    return statement;
}


static bool bsStatementsFromModel(BSValue statementModels, BSStatementList *list, BSScript *script,
                                  size_t *functionCapacity);


/* Convert a function definition statement's model */
static bool bsFunctionFromModel(BSValue model, BSStatement *statement, BSScript *script,
                                size_t *functionCapacity)
{
    BSValue name = bsObjectGet(model, "name");
    BSValue statements = bsObjectGet(model, "statements");
    if (name.type != BS_STRING || statements.type != BS_ARRAY) {
        return false;
    }

    BSFunctionDef *def = bsAlloc(sizeof(BSFunctionDef));
    memset(def, 0, sizeof(BSFunctionDef));
    def->name = bsInternName(name);
    def->lastArgArray = bsValueBoolean(bsObjectGet(model, "lastArgArray"));
    def->async = bsValueBoolean(bsObjectGet(model, "async"));
    def->lineNumber = statement->lineNumber;
    def->lineCount = statement->lineCount;
    def->script = script;
    statement->u.function.def = def;

    /* Register the definition with its script, which owns it */
    if (script->functionCount == *functionCapacity) {
        *functionCapacity = *functionCapacity != 0 ? *functionCapacity * 2 : 8;
        script->functions = bsRealloc(script->functions, *functionCapacity * sizeof(BSFunctionDef *));
    }
    script->functions[script->functionCount++] = def;

    BSValue args = bsObjectGet(model, "args");
    size_t argCount = bsArrayCount(args);
    if (argCount != 0) {
        def->argNames = bsAlloc(argCount * sizeof(BSValue));
        for (size_t ix = 0; ix < argCount; ix++) {
            BSValue argName = bsArrayGet(args, ix);
            if (argName.type != BS_STRING) {
                return false;
            }
            def->argNames[def->argCount++] = bsInternName(argName);
        }
    }

    BSStatementList list;
    memset(&list, 0, sizeof(list));
    bool valid = bsStatementsFromModel(statements, &list, script, functionCapacity);
    def->statements = list.statements;
    def->statementCount = list.count;
    return valid;
}


static bool bsStatementsFromModel(BSValue statementModels, BSStatementList *list, BSScript *script,
                                  size_t *functionCapacity)
{
    size_t count = bsArrayCount(statementModels);
    for (size_t ix = 0; ix < count; ix++) {
        BSValue model = bsArrayGet(statementModels, ix);
        if (model.type != BS_OBJECT) {
            return false;
        }

        BSValue value = bsObjectGet(model, "expr");
        if (value.type == BS_OBJECT) {
            BSExpr *expr = bsExprFromModel(bsObjectGet(value, "expr"));
            if (expr == NULL) {
                return false;
            }
            BSStatement *statement = bsStatementAdd(list, BS_STMT_EXPR, value);
            statement->u.expr.expr = expr;
            BSValue name = bsObjectGet(value, "name");
            if (name.type == BS_STRING) {
                statement->u.expr.name = bsInternName(name);
            }
            continue;
        }

        value = bsObjectGet(model, "jump");
        if (value.type == BS_OBJECT) {
            BSValue label = bsObjectGet(value, "label");
            if (label.type != BS_STRING) {
                return false;
            }
            BSExpr *expr = NULL;
            if (bsObjectHas(value, "expr")) {
                expr = bsExprFromModel(bsObjectGet(value, "expr"));
                if (expr == NULL) {
                    return false;
                }
            }
            BSStatement *statement = bsStatementAdd(list, BS_STMT_JUMP, value);
            statement->u.jump.label = bsRetain(label);
            statement->u.jump.expr = expr;
            continue;
        }

        value = bsObjectGet(model, "return");
        if (value.type == BS_OBJECT) {
            BSExpr *expr = NULL;
            if (bsObjectHas(value, "expr")) {
                expr = bsExprFromModel(bsObjectGet(value, "expr"));
                if (expr == NULL) {
                    return false;
                }
            }
            BSStatement *statement = bsStatementAdd(list, BS_STMT_RETURN, value);
            statement->u.ret.expr = expr;
            continue;
        }

        value = bsObjectGet(model, "label");
        if (value.type == BS_OBJECT) {
            BSValue name = bsObjectGet(value, "name");
            if (name.type != BS_STRING) {
                return false;
            }
            BSStatement *statement = bsStatementAdd(list, BS_STMT_LABEL, value);
            statement->u.label.name = bsRetain(name);
            continue;
        }

        value = bsObjectGet(model, "function");
        if (value.type == BS_OBJECT) {
            BSStatement *statement = bsStatementAdd(list, BS_STMT_FUNCTION, value);
            if (!bsFunctionFromModel(value, statement, script, functionCapacity)) {
                return false;
            }
            continue;
        }

        value = bsObjectGet(model, "include");
        if (value.type == BS_OBJECT) {
            BSValue includes = bsObjectGet(value, "includes");
            size_t includeCount = bsArrayCount(includes);
            if (includeCount == 0) {
                return false;
            }
            BSStatement *statement = bsStatementAdd(list, BS_STMT_INCLUDE, value);
            statement->u.include.includes = bsAlloc(includeCount * sizeof(BSInclude));
            for (size_t ixInclude = 0; ixInclude < includeCount; ixInclude++) {
                BSValue include = bsArrayGet(includes, ixInclude);
                BSValue url = bsObjectGet(include, "url");
                if (url.type != BS_STRING) {
                    return false;
                }
                statement->u.include.includes[statement->u.include.count].url = bsRetain(url);
                statement->u.include.includes[statement->u.include.count].system =
                    bsValueBoolean(bsObjectGet(include, "system"));
                statement->u.include.count++;
            }
            continue;
        }

        return false;
    }
    return true;
}


/*
 * Jump resolution - a jump's label becomes its statement index
 */


static void bsResolveJumps(BSStatement **statements, size_t statementCount)
{
    BSValue labels = bsObjectNew();
    for (size_t ix = 0; ix < statementCount; ix++) {
        if (statements[ix]->type == BS_STMT_LABEL) {
            bsObjectSetString(labels, statements[ix]->u.label.name, bsNumber((double) ix));
        }
    }
    for (size_t ix = 0; ix < statementCount; ix++) {
        BSStatement *statement = statements[ix];
        if (statement->type == BS_STMT_JUMP) {
            BSValue index = bsObjectGetString(labels, statement->u.jump.label);
            statement->u.jump.index = index.type == BS_NUMBER ? (int) index.u.number : -1;
        }
    }
    bsRelease(labels);
}


/*
 * Slot resolution
 *
 * A function's local variables are its declared arguments plus every assignment target in its body
 * - a statically known set, since BareScript has no dynamic local creation. Resolving each name to
 * a slot index turns a variable read into an array load.
 */


typedef struct BSSlotMap {
    BSValue names;  /* an object of name to slot index */
    BSValue *slots;
    size_t count;
    size_t capacity;
} BSSlotMap;


static int bsSlotFind(const BSSlotMap *map, BSValue name)
{
    BSValue index = bsObjectGetString(map->names, name);
    return index.type == BS_NUMBER ? (int) index.u.number : -1;
}


static void bsSlotAdd(BSSlotMap *map, BSValue name)
{
    if (bsSlotFind(map, name) >= 0) {
        return;
    }
    if (map->count == map->capacity) {
        map->capacity = map->capacity != 0 ? map->capacity * 2 : 16;
        map->slots = bsRealloc(map->slots, map->capacity * sizeof(BSValue));
    }
    bsObjectSetString(map->names, name, bsNumber((double) map->count));
    map->slots[map->count++] = bsRetain(name);
}


static void bsResolveExprSlots(BSExpr *expr, const BSSlotMap *map)
{
    switch (expr->type) {
    case BS_EXPR_VARIABLE:
        expr->u.variable.slot = bsSlotFind(map, expr->u.variable.name);
        break;
    case BS_EXPR_FUNCTION:
    case BS_EXPR_CALL0:
    case BS_EXPR_CALL1:
    case BS_EXPR_CALL2:
        expr->u.function.slot = bsSlotFind(map, expr->u.function.name);
        for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
            bsResolveExprSlots(expr->u.function.args[ix], map);
        }
        break;
    case BS_EXPR_BINARY:
        bsResolveExprSlots(expr->u.binary.left, map);
        bsResolveExprSlots(expr->u.binary.right, map);
        break;
    case BS_EXPR_UNARY:
        bsResolveExprSlots(expr->u.unary.expr, map);
        break;
    case BS_EXPR_GROUP:
        bsResolveExprSlots(expr->u.group, map);
        break;
    default:
        break;
    }
}


static void bsResolveSlots(BSFunctionDef *def)
{
    BSSlotMap map;
    memset(&map, 0, sizeof(map));
    map.names = bsObjectNew();

    /* The declared arguments come first, so an argument's slot is its position */
    for (size_t ix = 0; ix < def->argCount; ix++) {
        bsSlotAdd(&map, def->argNames[ix]);
    }
    for (size_t ix = 0; ix < def->statementCount; ix++) {
        BSStatement *statement = def->statements[ix];
        if (statement->type == BS_STMT_EXPR && statement->u.expr.name.type == BS_STRING) {
            bsSlotAdd(&map, statement->u.expr.name);
        }
    }

    for (size_t ix = 0; ix < def->statementCount; ix++) {
        BSStatement *statement = def->statements[ix];
        switch (statement->type) {
        case BS_STMT_EXPR:
            if (statement->u.expr.name.type == BS_STRING) {
                statement->u.expr.slot = bsSlotFind(&map, statement->u.expr.name);
            }
            bsResolveExprSlots(statement->u.expr.expr, &map);
            break;
        case BS_STMT_JUMP:
            if (statement->u.jump.expr != NULL) {
                bsResolveExprSlots(statement->u.jump.expr, &map);
            }
            break;
        case BS_STMT_RETURN:
            if (statement->u.ret.expr != NULL) {
                bsResolveExprSlots(statement->u.ret.expr, &map);
            }
            break;
        default:
            break;
        }
    }

    bsRelease(map.names);
    def->slotNames = map.slots;
    def->slotCount = map.count;
}


BSScript *bsScriptFromModel(BSValue model, const char *scriptName)
{
    BSValue statements = bsObjectGet(model, "statements");
    if (model.type != BS_OBJECT || statements.type != BS_ARRAY) {
        return NULL;
    }

    BSScript *script = bsAlloc(sizeof(BSScript));
    memset(script, 0, sizeof(BSScript));
    script->refcount = 1;
    script->system = bsValueBoolean(bsObjectGet(model, "system"));

    BSValue modelName = bsObjectGet(model, "scriptName");
    if (scriptName != NULL) {
        script->scriptName = bsStringNew(scriptName);
    } else {
        script->scriptName = modelName.type == BS_STRING ? bsRetain(modelName) : bsNull();
    }
    BSValue scriptLines = bsObjectGet(model, "scriptLines");
    script->scriptLines = scriptLines.type == BS_ARRAY ? bsRetain(scriptLines) : bsArrayNew();

    BSStatementList list;
    memset(&list, 0, sizeof(list));
    size_t functionCapacity = 0;
    bool valid = bsStatementsFromModel(statements, &list, script, &functionCapacity);
    script->statements = list.statements;
    script->statementCount = list.count;
    if (!valid) {
        bsScriptRelease(script);
        return NULL;
    }

    bsResolveJumps(script->statements, script->statementCount);
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        BSFunctionDef *def = script->functions[ix];
        bsResolveJumps(def->statements, def->statementCount);
        bsResolveSlots(def);
    }
    return script;
}


/*
 * Compiled representation to model
 */


BSValue bsExprToModel(const BSExpr *expr)
{
    BSValue model = bsObjectNew();
    switch (expr->type) {
    case BS_EXPR_NUMBER:
        bsObjectSet(model, "number", bsNumber(expr->u.number));
        break;

    case BS_EXPR_STRING:
        bsObjectSet(model, "string", bsRetain(expr->u.string));
        break;

    case BS_EXPR_VARIABLE:
        bsObjectSet(model, "variable", bsRetain(expr->u.variable.name));
        break;

    case BS_EXPR_FUNCTION:
    case BS_EXPR_CALL0:
    case BS_EXPR_CALL1:
    case BS_EXPR_CALL2: {
        BSValue function = bsObjectNew();
        bsObjectSet(function, "name", bsRetain(expr->u.function.name));
        BSValue args = bsArrayNewCapacity(expr->u.function.argCount);
        for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
            bsArrayPush(args, bsExprToModel(expr->u.function.args[ix]));
        }
        bsObjectSet(function, "args", args);
        bsObjectSet(model, "function", function);
        break;
    }

    case BS_EXPR_BINARY: {
        BSValue binary = bsObjectNew();
        bsObjectSet(binary, "op", bsStringNew(bsBinaryOpText[expr->u.binary.op]));
        bsObjectSet(binary, "left", bsExprToModel(expr->u.binary.left));
        bsObjectSet(binary, "right", bsExprToModel(expr->u.binary.right));
        bsObjectSet(model, "binary", binary);
        break;
    }

    case BS_EXPR_UNARY: {
        BSValue unary = bsObjectNew();
        bsObjectSet(unary, "op", bsStringNew(bsUnaryOpText[expr->u.unary.op]));
        bsObjectSet(unary, "expr", bsExprToModel(expr->u.unary.expr));
        bsObjectSet(model, "unary", unary);
        break;
    }

    default:
        bsObjectSet(model, "group", bsExprToModel(expr->u.group));
        break;
    }
    return model;
}


BSValue bsStatementToModel(const BSStatement *statement)
{
    BSValue model = bsObjectNew();
    BSValue value = bsObjectNew();
    if (statement->lineNumber != 0) {
        bsObjectSet(value, "lineNumber", bsNumber(statement->lineNumber));
    }
    if (statement->lineCount != 0) {
        bsObjectSet(value, "lineCount", bsNumber(statement->lineCount));
    }

    switch (statement->type) {
    case BS_STMT_EXPR:
        if (statement->u.expr.name.type == BS_STRING) {
            bsObjectSet(value, "name", bsRetain(statement->u.expr.name));
        }
        bsObjectSet(value, "expr", bsExprToModel(statement->u.expr.expr));
        bsObjectSet(model, "expr", value);
        break;

    case BS_STMT_JUMP:
        bsObjectSet(value, "label", bsRetain(statement->u.jump.label));
        if (statement->u.jump.expr != NULL) {
            bsObjectSet(value, "expr", bsExprToModel(statement->u.jump.expr));
        }
        bsObjectSet(model, "jump", value);
        break;

    case BS_STMT_RETURN:
        if (statement->u.ret.expr != NULL) {
            bsObjectSet(value, "expr", bsExprToModel(statement->u.ret.expr));
        }
        bsObjectSet(model, "return", value);
        break;

    case BS_STMT_LABEL:
        bsObjectSet(value, "name", bsRetain(statement->u.label.name));
        bsObjectSet(model, "label", value);
        break;

    case BS_STMT_FUNCTION: {
        const BSFunctionDef *def = statement->u.function.def;
        if (def->async) {
            bsObjectSet(value, "async", bsBoolean(true));
        }
        bsObjectSet(value, "name", bsRetain(def->name));
        if (def->argCount != 0) {
            BSValue args = bsArrayNewCapacity(def->argCount);
            for (size_t ix = 0; ix < def->argCount; ix++) {
                bsArrayPush(args, bsRetain(def->argNames[ix]));
            }
            bsObjectSet(value, "args", args);
        }
        if (def->lastArgArray) {
            bsObjectSet(value, "lastArgArray", bsBoolean(true));
        }
        BSValue statements = bsArrayNewCapacity(def->statementCount);
        for (size_t ix = 0; ix < def->statementCount; ix++) {
            bsArrayPush(statements, bsStatementToModel(def->statements[ix]));
        }
        bsObjectSet(value, "statements", statements);
        bsObjectSet(model, "function", value);
        break;
    }

    default: {
        BSValue includes = bsArrayNewCapacity(statement->u.include.count);
        for (size_t ix = 0; ix < statement->u.include.count; ix++) {
            BSValue include = bsObjectNew();
            bsObjectSet(include, "url", bsRetain(statement->u.include.includes[ix].url));
            if (statement->u.include.includes[ix].system) {
                bsObjectSet(include, "system", bsBoolean(true));
            }
            bsArrayPush(includes, include);
        }
        bsObjectSet(value, "includes", includes);
        bsObjectSet(model, "include", value);
        break;
    }
    }
    return model;
}


BSValue bsScriptToModel(const BSScript *script)
{
    BSValue model = bsObjectNew();
    BSValue statements = bsArrayNewCapacity(script->statementCount);
    for (size_t ix = 0; ix < script->statementCount; ix++) {
        bsArrayPush(statements, bsStatementToModel(script->statements[ix]));
    }
    bsObjectSet(model, "statements", statements);
    if (script->scriptName.type == BS_STRING) {
        bsObjectSet(model, "scriptName", bsRetain(script->scriptName));
    }
    if (bsArrayCount(script->scriptLines) != 0) {
        bsObjectSet(model, "scriptLines", bsRetain(script->scriptLines));
    }
    if (script->system) {
        bsObjectSet(model, "system", bsBoolean(true));
    }
    return model;
}
