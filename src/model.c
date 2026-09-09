/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript model conversion
 *
 * The parser produces a BareScript model - objects - and the bundled include library arrives as a
 * binary model. This file loads either into a transient syntax tree a statement at a time and
 * compiles it to bytecode, keeping a parsed script's model on the script for lint and coverage.
 * Jump labels become instruction indexes and function-local names become slot indexes during emit.
 */

#include <stdlib.h>

#include "barescript/json.h"

#include "internal.h"



static void bsCodeFree(BSCode *code)
{
    free(code->inst);
    for (size_t ix = 0; ix < code->constantCount; ix++) {
        bsRelease(code->constants[ix]);
    }
    free(code->constants);
    for (size_t ix = 0; ix < code->nameCount; ix++) {
        bsRelease(code->names[ix]);
    }
    free(code->names);
    free(code->caches);
    for (size_t ix = 0; ix < code->includeCount; ix++) {
        bsRelease(code->includes[ix].url);
    }
    free(code->includes);
    free(code->cover);
    free(code->coverLines);
    free(code->coverPcs);
    for (size_t ix = 0; ix < code->slotCount; ix++) {
        bsRelease(code->slotNames[ix]);
    }
    free(code->slotNames);
    memset(code, 0, sizeof(*code));
}


void bsExprFree(BSExpr *expr)
{
    if (expr == NULL) {
        return;
    }
    bsCodeFree(&expr->code);
    bsRelease(expr->model);
    free(expr);
}


static void bsFunctionDefFree(BSFunctionDef *def)
{
    bsRelease(def->name);
    bsCodeFree(&def->code);
    free(def->frame);
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
    bsCodeFree(&script->code);
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        bsFunctionDefFree(script->functions[ix]);
    }
    free(script->functions);
    bsRelease(script->scriptName);
    bsRelease(script->scriptLines);
    bsRelease(script->model);
    free(script->coverageCounts);
    free(script);
}


/*
 * The model keys
 */


/*
 * The model keys, interned once so every lookup compares pointers. The JSON decoder and the
 * parser script produce interned keys, so a model object's keys are never compared by content.
 */
static _Thread_local struct {
    BSValue args, binary, expr, function, group, include, includes, jump, label, lastArgArray, left, lineNumber, name, number, op, return_, right, scriptLines, scriptName, statements, string, system, unary, url, variable;
} bsKeys;


void bsModelKeysInit(void)
{
    /*
     * Interned once per thread - and again after a runtime cleanup, which leaves the strings held
     * here as ordinary ones that no longer compare by pointer
     */
    if (bsKeys.expr.type == BS_STRING) {
        if ((bsKeys.expr.u.string->flags & BS_STR_INTERNED) != 0) {
            return;
        }
        BSValue *keys = (BSValue *) &bsKeys;
        for (size_t ix = 0; ix < sizeof(bsKeys) / sizeof(BSValue); ix++) {
            bsRelease(keys[ix]);
        }
    }
#define BS_KEY(field, text) bsKeys.field = bsStringInternLiteral(text)
    BS_KEY(args, "args");
    BS_KEY(binary, "binary");
    BS_KEY(expr, "expr");
    BS_KEY(function, "function");
    BS_KEY(group, "group");
    BS_KEY(include, "include");
    BS_KEY(includes, "includes");
    BS_KEY(jump, "jump");
    BS_KEY(label, "label");
    BS_KEY(lastArgArray, "lastArgArray");
    BS_KEY(left, "left");
    BS_KEY(lineNumber, "lineNumber");
    BS_KEY(name, "name");
    BS_KEY(number, "number");
    BS_KEY(op, "op");
    BS_KEY(return_, "return");
    BS_KEY(right, "right");
    BS_KEY(scriptLines, "scriptLines");
    BS_KEY(scriptName, "scriptName");
    BS_KEY(statements, "statements");
    BS_KEY(string, "string");
    BS_KEY(system, "system");
    BS_KEY(unary, "unary");
    BS_KEY(url, "url");
    BS_KEY(variable, "variable");
#undef BS_KEY
    /* Parser-model keys the emitter does not read; intern so JSON decode reuses them */
    bsRelease(bsStringInternLiteral("async"));
    bsRelease(bsStringInternLiteral("lineCount"));
}


/* Retain an interned name; intern an ordinary one. Model strings are interned on decode. */
static BSValue bsInternName(BSValue name)
{
    if ((name.u.string->flags & BS_STR_INTERNED) != 0) {
        return bsRetain(name);
    }
    return bsStringIntern(bsStringSpan(name), bsStringSize(name));
}


/*
 * The syntax tree
 *
 * A statement is loaded into an arena of nodes - from the parser's model objects, or straight
 * from a bundled include's binary model, which then builds no objects at all - emitted, and the
 * arena reset for the next, so the arena holds one statement at a time. A node's links are indexes into the arena,
 * zero meaning none; its string is an index plus one into the arena's interned strings.
 */
enum {
    BS_NODE_NUMBER = 1,   /* number */
    BS_NODE_STRING,       /* text: the literal */
    BS_NODE_VARIABLE,     /* text: the name */
    BS_NODE_CALL,         /* text: the function name; a: the first argument; b: the argument count */
    BS_NODE_BINARY,       /* op: the opcode, or BS_NODE_AND / BS_NODE_OR; a, b: the operands */
    BS_NODE_UNARY,        /* op: the opcode; a: the operand */
    BS_NODE_GROUP,        /* a: the expression */
    BS_NODE_EXPR,         /* a: the expression; text: the name assigned, or none */
    BS_NODE_JUMP,         /* text: the label; a: the condition, or none */
    BS_NODE_RETURN,       /* a: the expression, or none */
    BS_NODE_LABEL,        /* text: the name */
    BS_NODE_FUNCTION,     /* text: the name; a: the first statement; b: the first argument; flag: lastArgArray */
    BS_NODE_INCLUDE,      /* a: the first include */
    BS_NODE_INCLUDE_ITEM, /* text: the url; flag: system */
    BS_NODE_ARG           /* text: a function argument's name */
};

/* The short-circuit operators, in a binary node's op past the opcodes */
#define BS_NODE_AND 0xFE
#define BS_NODE_OR 0xFF

typedef struct BSNode {
    uint8_t kind;
    uint8_t flag;
    uint16_t op;
    uint32_t next;  /* the next node of a list, or zero */
    uint32_t a;
    uint32_t b;
    uint32_t text;
    int32_t line;   /* a statement's line number, or zero */
    double number;
    BSValue model;  /* a statement's model object, borrowed, for coverage - or a null value */
} BSNode;

typedef struct BSAst {
    BSNode *nodes;   /* node zero is unused, so a zero link means none */
    uint32_t count;
    uint32_t capacity;
    BSValue *strings;
    uint32_t stringCount;
    uint32_t stringCapacity;
} BSAst;


#define BS_AST_INITIAL 64


static void bsAstInit(BSAst *ast)
{
    memset(ast, 0, sizeof(*ast));
    ast->count = 1;
}


static void bsAstReset(BSAst *ast)
{
    for (uint32_t ix = 0; ix < ast->stringCount; ix++) {
        bsRelease(ast->strings[ix]);
    }
    ast->stringCount = 0;
    ast->count = 1;
}


static void bsAstFree(BSAst *ast)
{
    bsAstReset(ast);
    free(ast->nodes);
    free(ast->strings);
    memset(ast, 0, sizeof(*ast));
}


static uint32_t bsAstNode(BSAst *ast, uint8_t kind)
{
    BS_GROW(ast->nodes, ast->count, ast->capacity, BS_AST_INITIAL);
    BSNode *node = &ast->nodes[ast->count];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    return ast->count++;
}


static uint32_t bsAstString(BSAst *ast, BSValue string)
{
    BS_GROW(ast->strings, ast->stringCount, ast->stringCapacity, 16);
    ast->strings[ast->stringCount] = string;
    return ++ast->stringCount;
}


/* A node's string, borrowed */
static inline BSValue bsNodeText(const BSAst *ast, const BSNode *node)
{
    return ast->strings[node->text - 1];
}


/* Whether a name is the word - a first-character test before the compare, the names rarely being it */
static inline bool bsNameIs(const char *name, const char *word)
{
    return name[0] == word[0] && strcmp(name, word) == 0;
}


/* The same of a string value, by size and bytes, so a slice is read as it is */
static inline bool bsStringIs(BSValue value, const char *word)
{
    size_t size = strlen(word);
    return value.u.string->size == size && memcmp(value.u.string->data, word, size) == 0;
}


/* Append a node to the list whose first node is "*head" and last node "*tail" */
static void bsAstAppend(BSAst *ast, uint32_t *head, uint32_t *tail, uint32_t node)
{
    if (*head == 0) {
        *head = node;
    } else {
        ast->nodes[*tail].next = node;
    }
    *tail = node;
}


/*
 * A model node - an expression or a statement - is an object with one member whose name is its
 * kind. The kind key, with the member in "*member", or NULL for a value of any other shape.
 */
static BSString *bsModelKind(BSValue node, BSValue *member)
{
    if (node.type != BS_OBJECT || node.u.object->count != 1) {
        *member = bsNull();
        return NULL;
    }
    *member = node.u.object->entries->value;
    return node.u.object->entries->key;
}

/* Whether a node's kind key is the model key "field" */
#define BS_KIND(kind, field) ((kind) != NULL && bsObjectKeyIs((kind), bsKeys.field))


/* A binary node's op: the operator's opcode, or the short-circuit operators' own codes; zero if unknown */
static uint16_t bsBinaryNodeOp(const char *op)
{
    char first = op[0];
    char second = first != '\0' ? op[1] : '\0';
    if (second == '\0') {
        switch (first) {
        case '*':
            return BS_OP_MUL;
        case '/':
            return BS_OP_DIV;
        case '%':
            return BS_OP_MOD;
        case '+':
            return BS_OP_ADD;
        case '-':
            return BS_OP_SUB;
        case '<':
            return BS_OP_LT;
        case '>':
            return BS_OP_GT;
        case '&':
            return BS_OP_BAND;
        case '^':
            return BS_OP_BXOR;
        case '|':
            return BS_OP_BOR;
        default:
            return 0;
        }
    }
    if (op[2] != '\0') {
        return 0;
    }
    if (second == '=') {
        return first == '<' ? BS_OP_LE : first == '>' ? BS_OP_GE : first == '=' ? BS_OP_EQ : first == '!' ? BS_OP_NE : 0;
    }
    if (first == second) {
        return first == '*' ? BS_OP_POW : first == '<' ? BS_OP_SHL : first == '>' ? BS_OP_SHR :
            first == '&' ? BS_NODE_AND : first == '|' ? BS_NODE_OR : 0;
    }
    return 0;
}


static uint8_t bsUnaryOpcode(const char *op)
{
    if (op[0] != '\0' && op[1] == '\0') {
        if (op[0] == '-') {
            return BS_OP_NEG;
        }
        if (op[0] == '!') {
            return BS_OP_NOT;
        }
        if (op[0] == '~') {
            return BS_OP_BNOT;
        }
    }
    return 0;
}


static uint32_t bsAstExpr(BSAst *ast, BSValue model)
{
    BSValue member;
    BSString *kind = bsModelKind(model, &member);
    if (BS_KIND(kind, number)) {
        if (member.type != BS_NUMBER) {
            return 0;
        }
        uint32_t node = bsAstNode(ast, BS_NODE_NUMBER);
        ast->nodes[node].number = member.u.number;
        return node;
    }
    if (BS_KIND(kind, string) || BS_KIND(kind, variable)) {
        if (member.type != BS_STRING) {
            return 0;
        }
        uint32_t node = bsAstNode(ast, BS_KIND(kind, string) ? BS_NODE_STRING : BS_NODE_VARIABLE);
        ast->nodes[node].text = bsAstString(ast, bsInternName(member));
        return node;
    }
    if (BS_KIND(kind, group)) {
        uint32_t sub = bsAstExpr(ast, member);
        if (sub == 0) {
            return 0;
        }
        uint32_t node = bsAstNode(ast, BS_NODE_GROUP);
        ast->nodes[node].a = sub;
        return node;
    }
    if (member.type != BS_OBJECT) {
        return 0;
    }
    if (BS_KIND(kind, function)) {
        BSValue name = bsObjectGetString(member, bsKeys.name);
        if (name.type != BS_STRING) {
            return 0;
        }
        /* The conditional reads its first three arguments and no more, so a fourth of any shape stands */
        BSValue args = bsObjectGetString(member, bsKeys.args);
        size_t argCount = bsArrayCount(args);
        if (argCount > 3 && bsStringIs(name, "if")) {
            argCount = 3;
        }
        uint32_t node = bsAstNode(ast, BS_NODE_CALL);
        ast->nodes[node].text = bsAstString(ast, bsInternName(name));
        ast->nodes[node].b = (uint32_t) argCount;
        uint32_t tail = 0;
        for (size_t ix = 0; ix < argCount; ix++) {
            uint32_t arg = bsAstExpr(ast, bsArrayGet(args, ix));
            if (arg == 0) {
                return 0;
            }
            bsAstAppend(ast, &ast->nodes[node].a, &tail, arg);
        }
        return node;
    }
    if (BS_KIND(kind, binary)) {
        BSValue op = bsObjectGetString(member, bsKeys.op);
        uint16_t nodeOp = op.type == BS_STRING ? bsBinaryNodeOp(bsStringData(op)) : 0;
        if (nodeOp == 0) {
            return 0;
        }
        uint32_t left = bsAstExpr(ast, bsObjectGetString(member, bsKeys.left));
        uint32_t right = left != 0 ? bsAstExpr(ast, bsObjectGetString(member, bsKeys.right)) : 0;
        if (right == 0) {
            return 0;
        }
        uint32_t node = bsAstNode(ast, BS_NODE_BINARY);
        ast->nodes[node].op = nodeOp;
        ast->nodes[node].a = left;
        ast->nodes[node].b = right;
        return node;
    }
    if (BS_KIND(kind, unary)) {
        BSValue op = bsObjectGetString(member, bsKeys.op);
        uint8_t opcode = op.type == BS_STRING ? bsUnaryOpcode(bsStringData(op)) : 0;
        if (opcode == 0) {
            return 0;
        }
        uint32_t sub = bsAstExpr(ast, bsObjectGetString(member, bsKeys.expr));
        if (sub == 0) {
            return 0;
        }
        uint32_t node = bsAstNode(ast, BS_NODE_UNARY);
        ast->nodes[node].op = opcode;
        ast->nodes[node].a = sub;
        return node;
    }
    return 0;
}


/* A statement's optional expression member, into "*expr" - zero when absent - or false for a malformed one */
static bool bsAstOptionalExpr(BSAst *ast, BSValue value, uint32_t *expr)
{
    *expr = 0;
    if (!bsObjectHasString(value, bsKeys.expr)) {
        return true;
    }
    *expr = bsAstExpr(ast, bsObjectGetString(value, bsKeys.expr));
    return *expr != 0;
}


static uint32_t bsAstStatement(BSAst *ast, BSValue model)
{
    BSValue value;
    BSString *kind = bsModelKind(model, &value);
    if (kind == NULL || value.type != BS_OBJECT) {
        return 0;
    }
    uint32_t node;
    uint32_t tail = 0;
    if (BS_KIND(kind, expr)) {
        uint32_t expr = bsAstExpr(ast, bsObjectGetString(value, bsKeys.expr));
        if (expr == 0) {
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_EXPR);
        ast->nodes[node].a = expr;
        BSValue name = bsObjectGetString(value, bsKeys.name);
        if (name.type == BS_STRING) {
            ast->nodes[node].text = bsAstString(ast, bsInternName(name));
        }
    } else if (BS_KIND(kind, jump)) {
        BSValue label = bsObjectGetString(value, bsKeys.label);
        if (label.type != BS_STRING) {
            return 0;
        }
        uint32_t cond;
        if (!bsAstOptionalExpr(ast, value, &cond)) {
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_JUMP);
        ast->nodes[node].a = cond;
        ast->nodes[node].text = bsAstString(ast, bsInternName(label));
    } else if (BS_KIND(kind, return_)) {
        uint32_t expr;
        if (!bsAstOptionalExpr(ast, value, &expr)) {
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_RETURN);
        ast->nodes[node].a = expr;
    } else if (BS_KIND(kind, label)) {
        BSValue name = bsObjectGetString(value, bsKeys.name);
        if (name.type != BS_STRING) {
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_LABEL);
        ast->nodes[node].text = bsAstString(ast, bsInternName(name));
    } else if (BS_KIND(kind, function)) {
        BSValue name = bsObjectGetString(value, bsKeys.name);
        BSValue statements = bsObjectGetString(value, bsKeys.statements);
        if (name.type != BS_STRING || statements.type != BS_ARRAY) {
            return 0;
        }
        BSValue args = bsObjectGetString(value, bsKeys.args);
        size_t argCount = bsArrayCount(args);
        for (size_t ix = 0; ix < argCount; ix++) {
            if (bsArrayGet(args, ix).type != BS_STRING) {
                return 0;
            }
        }
        node = bsAstNode(ast, BS_NODE_FUNCTION);
        ast->nodes[node].text = bsAstString(ast, bsInternName(name));
        ast->nodes[node].flag = bsValueBoolean(bsObjectGetString(value, bsKeys.lastArgArray));
        for (size_t ix = 0; ix < argCount; ix++) {
            uint32_t arg = bsAstNode(ast, BS_NODE_ARG);
            ast->nodes[arg].text = bsAstString(ast, bsInternName(bsArrayGet(args, ix)));
            bsAstAppend(ast, &ast->nodes[node].b, &tail, arg);
        }
        tail = 0;
        size_t count = bsArrayCount(statements);
        for (size_t ix = 0; ix < count; ix++) {
            uint32_t statement = bsAstStatement(ast, bsArrayGet(statements, ix));
            if (statement == 0) {
                return 0;
            }
            bsAstAppend(ast, &ast->nodes[node].a, &tail, statement);
        }
    } else if (BS_KIND(kind, include)) {
        BSValue includes = bsObjectGetString(value, bsKeys.includes);
        size_t count = bsArrayCount(includes);
        if (count == 0) {
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_INCLUDE);
        for (size_t ix = 0; ix < count; ix++) {
            BSValue include = bsArrayGet(includes, ix);
            BSValue url = bsObjectGetString(include, bsKeys.url);
            if (url.type != BS_STRING) {
                return 0;
            }
            uint32_t item = bsAstNode(ast, BS_NODE_INCLUDE_ITEM);
            ast->nodes[item].text = bsAstString(ast, bsInternName(url));
            ast->nodes[item].flag = bsValueBoolean(bsObjectGetString(include, bsKeys.system));
            bsAstAppend(ast, &ast->nodes[node].a, &tail, item);
        }
    } else {
        return 0;
    }
    BSValue line = bsObjectGetString(value, bsKeys.lineNumber);
    ast->nodes[node].line = line.type == BS_NUMBER ? (int) line.u.number : 0;
    ast->nodes[node].model = model;
    return node;
}


/*
 * Code emission
 *
 * A chunk's registers are its named locals - the function's arguments and assigned names, each a
 * slot - followed by its temporaries, allocated stack-fashion as an expression is compiled: a
 * subexpression's result lands in the lowest free temporary, and the temporaries a subexpression
 * used are free again once its result has been consumed. An operand names a register or, with
 * the high bit set, a constant, so a local or a literal feeds an operator or a call with no
 * instruction of its own.
 */


typedef struct {
    size_t pc;
    BSValue label;
} BSPatch;


typedef uint16_t BSOperand;

typedef struct {
    BSInst *inst;
    size_t count;
    size_t cap;
    BSValue *constants;
    size_t constCount;
    size_t constCap;
    BSValue *names;       /* the name sites' and unknown-label traps' names, interned */
    size_t nameCount;
    size_t nameCap;
    BSValue *cover;
    int *coverLines;
    uint32_t *coverPcs;
    size_t coverCount;
    size_t coverCap;
    BSInclude *includes;
    size_t includeCount;
    size_t includeCap;
    BSCallCache *caches;  /* per CALL_NAME, LOAD_NAME, or STORE_NAME site */
    size_t cacheCount;
    size_t cacheCap;
    uint16_t tempTop;     /* the temporaries in use past the slots */
    uint16_t tempMax;
    BSOperand trueConst;  /* the true and false constants' operands, or zero until allocated */
    BSOperand falseConst;
    bool overflow;        /* an operand space outgrew its index range, so the chunk is invalid */
    size_t assignedWords; /* the definite-assignment sets, slotCount bits each, or 0 for no analysis */
    uint32_t *assigned;   /* the slots definitely assigned at the statement being emitted */
    uint32_t *blockOf;    /* per statement, its basic block */
    uint32_t *blockIn;    /* per block, the slots definitely assigned on entry */
    BSValue *slotNames;
    size_t slotCount;
    size_t slotCap;
    BSValue slotMap;
    BSValue constMap;     /* interned constant string -> constant index */
    BSValue nameMap;      /* interned name -> name index */
    BSValue labels;
    BSPatch *patches;
    size_t patchCount;
    size_t patchCap;
    BSScript *script;
    size_t *functionCap;
} BSEmit;


/* The null constant's operand: constant zero, which bsEmitInit allocates first */
#define BS_OPERAND_NULL BS_OPERAND_CONST


static uint32_t bsEmitInst(BSEmit *e, uint8_t op, uint16_t a, uint16_t b, uint16_t c)
{
    BS_GROW(e->inst, e->count, e->cap, 32);
    BSInst *inst = &e->inst[e->count];
    inst->op = op;
    inst->a = a;
    inst->b = b;
    inst->c = c;
    return (uint32_t) e->count++;
}


static uint32_t bsEmitJumpInst(BSEmit *e, uint8_t op, uint16_t a, uint32_t target)
{
    uint32_t pc = bsEmitInst(e, op, a, 0, 0);
    e->inst[pc].w = target;
    return pc;
}


/*
 * The operand spaces - constants, temporaries, name sites, includes, functions - are bounded by
 * their operand's width. An allocation past the bound marks the chunk overflowed rather than
 * failing at its site; the operand it returns is then meaningless, but nothing reads it before
 * bsEmitFinish rejects the chunk.
 */

/* Mark the chunk overflowed when an operand space's count passes its bound */
static inline void bsEmitLimit(BSEmit *e, size_t count, size_t max)
{
    if (count > max) {
        e->overflow = true;
    }
}


/* One of the emitter's lookup objects, created on its first store - the reads tolerate a null value */
static BSValue bsEmitMap(BSValue *map)
{
    if (map->type != BS_OBJECT) {
        *map = bsObjectNew();
    }
    return *map;
}


/* A constant's operand. Interned strings - literals - are shared, so a chunk holds each once. */
static BSOperand bsEmitConst(BSEmit *e, BSValue value)
{
    bool interned = value.type == BS_STRING && (value.u.string->flags & BS_STR_INTERNED) != 0;
    if (interned) {
        BSValue index = bsObjectGetString(bsEmitMap(&e->constMap), value);
        if (index.type == BS_NUMBER) {
            return (BSOperand) (BS_OPERAND_CONST | (uint32_t) index.u.number);
        }
        bsObjectSetString(e->constMap, value, bsNumber((double) e->constCount));
    }
    bsEmitLimit(e, e->constCount, BS_OPERAND_MAX);
    BS_GROW(e->constants, e->constCount, e->constCap, 16);
    e->constants[e->constCount] = bsRetain(value);
    return (BSOperand) (BS_OPERAND_CONST | (uint32_t) e->constCount++);
}


/* A name's index in the chunk's names, each held once - the name is interned */
static uint16_t bsEmitName(BSEmit *e, BSValue name)
{
    BSValue index = bsObjectGetString(bsEmitMap(&e->nameMap), name);
    if (index.type == BS_NUMBER) {
        return (uint16_t) index.u.number;
    }
    bsEmitLimit(e, e->nameCount, BS_INDEX_MAX);
    bsObjectSetString(e->nameMap, name, bsNumber((double) e->nameCount));
    BS_GROW(e->names, e->nameCount, e->nameCap, 8);
    e->names[e->nameCount] = bsRetain(name);
    return (uint16_t) e->nameCount++;
}


/* Start a chunk's emit. Constant zero is null, so the null operand never needs allocating. */
static void bsEmitInit(BSEmit *e, BSScript *script, size_t *functionCap)
{
    memset(e, 0, sizeof(*e));
    e->script = script;
    e->functionCap = functionCap;
    bsEmitConst(e, bsNull());
}


static BSOperand bsEmitBool(BSEmit *e, bool value)
{
    BSOperand *operand = value ? &e->trueConst : &e->falseConst;
    if (*operand == 0) {
        *operand = bsEmitConst(e, bsBoolean(value));
    }
    return *operand;
}


/* Allocate the next temporary register */
static uint16_t bsTempAlloc(BSEmit *e)
{
    size_t index = e->slotCount + e->tempTop;
    bsEmitLimit(e, index, BS_OPERAND_MAX);
    e->tempTop++;
    if (e->tempTop > e->tempMax) {
        e->tempMax = e->tempTop;
    }
    return (uint16_t) index;
}


static int bsSlotFind(const BSEmit *e, BSValue name)
{
    BSValue index = bsObjectGetString(e->slotMap, name);
    return index.type == BS_NUMBER ? (int) index.u.number : -1;
}


/*
 * Add a slot for a name. An argument gets a slot of its own even when its name repeats, so a call
 * fills argCount slots; the name is then the last argument's, as in the references.
 */
static void bsSlotAdd(BSEmit *e, BSValue name, bool argument)
{
    if (!argument && bsSlotFind(e, name) >= 0) {
        return;
    }
    BS_GROW(e->slotNames, e->slotCount, e->slotCap, 8);
    bsObjectSetString(bsEmitMap(&e->slotMap), name, bsNumber((double) e->slotCount));
    e->slotNames[e->slotCount++] = bsRetain(name);
}


/* Allocate a global-name cache site for a name instruction; the operand is its index */
static uint16_t bsEmitSite(BSEmit *e, BSValue name)
{
    bsEmitLimit(e, e->cacheCount, BS_OPERAND_MAX);
    BS_GROW(e->caches, e->cacheCount, e->cacheCap, 8);
    BSCallCache *cache = &e->caches[e->cacheCount];
    memset(cache, 0, sizeof(*cache));
    cache->nameIndex = bsEmitName(e, name);
    return (uint16_t) e->cacheCount++;
}


static void bsEmitLabel(BSEmit *e, BSValue name)
{
    bsObjectSetString(bsEmitMap(&e->labels), name, bsNumber((double) e->count));
}


/* Point the jump word at "at" to a label - now if the label is behind, at the chunk's end if ahead */
static void bsEmitJumpLabel(BSEmit *e, uint32_t at, BSValue label)
{
    BSValue pc = bsObjectGetString(e->labels, label);
    if (pc.type == BS_NUMBER) {
        e->inst[at].w = (uint32_t) pc.u.number;
        return;
    }
    BS_GROW(e->patches, e->patchCount, e->patchCap, 8);
    e->patches[e->patchCount].pc = at;
    e->patches[e->patchCount].label = bsRetain(label);
    e->patchCount++;
}


/* The jump words a condition emits, to point at a target once it is known - a few on the stack */
typedef struct BSJumps {
    uint32_t *pcs;
    size_t count;
    size_t cap;
    uint32_t inline_[8];
} BSJumps;

static void bsJumpsInit(BSJumps *jumps)
{
    jumps->pcs = jumps->inline_;
    jumps->count = 0;
    jumps->cap = sizeof(jumps->inline_) / sizeof(jumps->inline_[0]);
}

static void bsJumpsAdd(BSJumps *jumps, uint32_t pc)
{
    if (jumps->count == jumps->cap) {
        uint32_t *pcs = bsAlloc(jumps->cap * 2 * sizeof(uint32_t));
        memcpy(pcs, jumps->pcs, jumps->count * sizeof(uint32_t));
        if (jumps->pcs != jumps->inline_) {
            free(jumps->pcs);
        }
        jumps->pcs = pcs;
        jumps->cap *= 2;
    }
    jumps->pcs[jumps->count++] = pc;
}

static void bsJumpsFree(BSJumps *jumps)
{
    if (jumps->pcs != jumps->inline_) {
        free(jumps->pcs);
    }
}

/* Point a condition's jumps at an instruction index */
static void bsJumpsTarget(BSEmit *e, BSJumps *jumps, uint32_t target)
{
    for (size_t ix = 0; ix < jumps->count; ix++) {
        e->inst[jumps->pcs[ix]].w = target;
    }
    bsJumpsFree(jumps);
}

/* Point a condition's jumps at a label */
static void bsJumpsLabel(BSEmit *e, BSJumps *jumps, BSValue label)
{
    for (size_t ix = 0; ix < jumps->count; ix++) {
        bsEmitJumpLabel(e, jumps->pcs[ix], label);
    }
    bsJumpsFree(jumps);
}


static void bsEmitExprTo(BSEmit *e, const BSAst *ast, uint32_t id, uint16_t dst);


/*
 * Definite assignment
 *
 * A slot read before the slot is assigned falls through to the global of the same name, which
 * costs every register read a test for the unset marker. The emitter instead computes, for each
 * function body, which slots are definitely assigned at each statement - a forward must-analysis
 * over the body's basic blocks, which its labels and jumps delimit - and reads such slots as plain
 * registers; a read that might find the slot unset goes through LOAD_SLOT, which tests.
 *
 * A jump to a label defined more than once may reach any definition; a jump to an unknown label
 * traps and reaches nothing.
 */
static inline bool bsAssignedTest(const BSEmit *e, int slot)
{
    return (e->assigned[slot / 32] >> (slot % 32)) & 1u;
}


static inline void bsAssignedSet(BSEmit *e, int slot)
{
    e->assigned[slot / 32] |= (uint32_t) 1 << (slot % 32);
}


/* The name a statement assigns - an expression statement with a name - or a null value */
static BSValue bsStatementAssignName(const BSAst *ast, uint32_t statement)
{
    const BSNode *node = &ast->nodes[statement];
    return node->kind == BS_NODE_EXPR && node->text != 0 ? bsNodeText(ast, node) : bsNull();
}


static void bsAssignedAnalyze(BSEmit *e, const BSAst *ast, const uint32_t *statements, size_t count, size_t argCount)
{
    size_t words = (e->slotCount + 31) / 32;
    if (e->slotCount == 0 || count == 0) {
        return;
    }

    /* Blocks: the first statement, each label, and each statement after a jump or return start one */
    BSValue labelBlocks = bsObjectNew(); /* label name -> the array of block indexes that define it */
    uint32_t *blockOf = bsAlloc(count * sizeof(uint32_t));
    size_t blockCount = 0;
    bool starts = true;
    for (size_t ix = 0; ix < count; ix++) {
        const BSNode *node = &ast->nodes[statements[ix]];
        bool isLabel = node->kind == BS_NODE_LABEL;
        if (starts || isLabel) {
            blockCount++;
        }
        blockOf[ix] = (uint32_t) (blockCount - 1);
        starts = !isLabel && (node->kind == BS_NODE_JUMP || node->kind == BS_NODE_RETURN);
        if (isLabel) {
            BSValue name = bsNodeText(ast, node);
            BSValue blocks = bsObjectGetString(labelBlocks, name);
            if (blocks.type != BS_ARRAY) {
                blocks = bsArrayNew();
                bsObjectSetString(labelBlocks, name, blocks);
            }
            bsArrayPush(blocks, bsNumber((double) blockOf[ix]));
        }
    }

    /* Each block's assignments, and its successors: the next block unless it ends in a jump or return */
    uint32_t *gen = bsAlloc(blockCount * words * sizeof(uint32_t));
    memset(gen, 0, blockCount * words * sizeof(uint32_t));
    uint32_t *in = bsAlloc(blockCount * words * sizeof(uint32_t));
    memset(in, 0xff, blockCount * words * sizeof(uint32_t));
    memset(in, 0, words * sizeof(uint32_t));
    for (size_t ix = 0; ix < argCount; ix++) {
        in[ix / 32] |= (uint32_t) 1 << (ix % 32);
    }
    for (size_t ix = 0; ix < count; ix++) {
        BSValue name = bsStatementAssignName(ast, statements[ix]);
        int slot = name.type == BS_STRING ? bsSlotFind(e, name) : -1;
        if (slot >= 0) {
            gen[blockOf[ix] * words + (size_t) slot / 32] |= (uint32_t) 1 << (slot % 32);
        }
    }

    /* The fixed point: a block's entry set is the intersection of its predecessors' exit sets */
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t ix = 0; ix < count; ix++) {
            uint32_t block = blockOf[ix];
            bool last = ix + 1 == count || blockOf[ix + 1] != block;
            if (!last) {
                continue;
            }
            const uint32_t *blockGen = &gen[block * words];
            const uint32_t *blockIn = &in[block * words];
            const BSNode *node = &ast->nodes[statements[ix]];
            bool isJump = node->kind == BS_NODE_JUMP;
            bool fallsThrough = isJump ? node->a != 0 : node->kind != BS_NODE_RETURN;
            BSValue targets = isJump ? bsObjectGetString(labelBlocks, bsNodeText(ast, node)) : bsNull();
            size_t targetCount = targets.type == BS_ARRAY ? bsArrayCount(targets) : 0;
            for (size_t succIx = 0; succIx < targetCount + (fallsThrough ? 1 : 0); succIx++) {
                uint32_t succ = succIx < targetCount ? (uint32_t) bsArrayGet(targets, succIx).u.number :
                    block + 1;
                if (succ >= blockCount) {
                    continue;
                }
                uint32_t *succIn = &in[succ * words];
                for (size_t w = 0; w < words; w++) {
                    uint32_t next = succIn[w] & (blockIn[w] | blockGen[w]);
                    if (next != succIn[w]) {
                        succIn[w] = next;
                        changed = true;
                    }
                }
            }
        }
    }

    e->assignedWords = words;
    e->assigned = bsAlloc(words * sizeof(uint32_t));
    e->blockOf = blockOf;
    e->blockIn = in;
    free(gen);
    bsRelease(labelBlocks);
}


/*
 * Compile an expression to an operand: a constant or a local costs no instruction; anything else
 * is computed into the lowest free temporary, which stays allocated for the caller to consume.
 */
static BSOperand bsEmitExprOperand(BSEmit *e, const BSAst *ast, uint32_t id)
{
    const BSNode *node = &ast->nodes[id];
    switch (node->kind) {
    case BS_NODE_NUMBER:
        return bsEmitConst(e, bsNumber(node->number));
    case BS_NODE_STRING:
        return bsEmitConst(e, bsNodeText(ast, node));
    case BS_NODE_VARIABLE: {
        BSValue name = bsNodeText(ast, node);
        const char *text = bsStringData(name);
        if (bsNameIs(text, "null")) {
            return BS_OPERAND_NULL;
        }
        if (bsNameIs(text, "true") || bsNameIs(text, "false")) {
            return bsEmitBool(e, text[0] == 't');
        }
        int slot = bsSlotFind(e, name);
        if (slot >= 0 && bsAssignedTest(e, slot)) {
            return (BSOperand) slot;
        }
        break;
    }
    case BS_NODE_GROUP:
        return bsEmitExprOperand(e, ast, node->a);
    default:
        break;
    }

    /* A global or a possibly unset local loads into a temporary; a call or an operator computes into one */
    BSOperand temp = bsTempAlloc(e);
    bsEmitExprTo(e, ast, id, temp);
    return temp;
}


/* Compile an operand the next instruction consumes: its temporaries are free again once it is emitted */
static BSOperand bsEmitExprConsumed(BSEmit *e, const BSAst *ast, uint32_t id)
{
    uint16_t base = e->tempTop;
    BSOperand operand = bsEmitExprOperand(e, ast, id);
    e->tempTop = base;
    return operand;
}


/*
 * Accumulate into dst, or a temporary when dst is a named local whose new value must not be
 * visible to later operands. Pair with bsEmitAccStore.
 */
static uint16_t bsEmitAcc(BSEmit *e, uint16_t dst, uint16_t *base)
{
    *base = e->tempTop;
    return dst < e->slotCount ? bsTempAlloc(e) : dst;
}

static void bsEmitAccStore(BSEmit *e, uint16_t dst, uint16_t acc, uint16_t base)
{
    if (acc != dst) {
        bsEmitInst(e, BS_OP_MOVE, dst, acc, 0);
        e->tempTop = base;
    }
}


/* Compile the argument node "arg" into dst, or move null if there is no such argument */
static void bsEmitArgOrNull(BSEmit *e, const BSAst *ast, uint32_t arg, uint16_t dst)
{
    if (arg != 0) {
        bsEmitExprTo(e, ast, arg, dst);
    } else {
        bsEmitInst(e, BS_OP_MOVE, dst, BS_OPERAND_NULL, 0);
    }
}


/*
 * The comparison jump opcode for a jump on the expression "id" being "jumpIfTrue", when it is a
 * comparison - its operand nodes are returned - or zero. A jump on false takes the opposite
 * comparison: EQ and NE, LT and GE, LE and GT are the pairs.
 */
static uint8_t bsCompareJumpOpcode(const BSNode *node, bool jumpIfTrue)
{
    static const uint8_t opposite[] = {BS_OP_NE, BS_OP_EQ, BS_OP_GE, BS_OP_GT, BS_OP_LE, BS_OP_LT};
    if (node->kind != BS_NODE_BINARY || node->op < BS_OP_EQ || node->op > BS_OP_GE) {
        return 0;
    }
    uint8_t opcode = (uint8_t) node->op;
    if (!jumpIfTrue) {
        opcode = opposite[opcode - BS_OP_EQ];
    }
    return (uint8_t) (BS_OP_JUMP_EQ + (opcode - BS_OP_EQ));
}


/*
 * Compile a condition as the jumps that leave when its truth is "jumpIfTrue", collected in "jumps"
 * for the caller to point at the target, and fall through otherwise. A comparison emits its
 * comparison jump on the comparison's operands, with a DATA word for the target; "and" and "or"
 * short-circuit through jumps of their own, so each comparison in a chain is one jump; a "not"
 * flips the sense; any other condition computes into an operand and jumps on it.
 */
static void bsEmitCondition(BSEmit *e, const BSAst *ast, uint32_t expr, bool jumpIfTrue, BSJumps *jumps)
{
    const BSNode *node = &ast->nodes[expr];
    while (node->kind == BS_NODE_GROUP) {
        expr = node->a;
        node = &ast->nodes[expr];
    }
    if (node->kind == BS_NODE_UNARY && node->op == BS_OP_NOT) {
        bsEmitCondition(e, ast, node->a, !jumpIfTrue, jumps);
        return;
    }
    if (node->kind == BS_NODE_BINARY && (node->op == BS_NODE_AND || node->op == BS_NODE_OR)) {
        if ((node->op == BS_NODE_AND) == jumpIfTrue) {
            /* A left operand that settles the condition the other way skips past the right's jump */
            BSJumps skip;
            bsJumpsInit(&skip);
            bsEmitCondition(e, ast, node->a, !jumpIfTrue, &skip);
            bsEmitCondition(e, ast, node->b, jumpIfTrue, jumps);
            bsJumpsTarget(e, &skip, (uint32_t) e->count);
        } else {
            /* Either operand settling the condition this way jumps */
            bsEmitCondition(e, ast, node->a, jumpIfTrue, jumps);
            bsEmitCondition(e, ast, node->b, jumpIfTrue, jumps);
        }
        return;
    }

    uint16_t base = e->tempTop;
    uint8_t compareJump = bsCompareJumpOpcode(node, jumpIfTrue);
    if (compareJump != 0) {
        BSOperand leftOperand = bsEmitExprOperand(e, ast, node->a);
        BSOperand rightOperand = bsEmitExprOperand(e, ast, node->b);
        e->tempTop = base;
        bsEmitInst(e, compareJump, 0, leftOperand, rightOperand);
        bsJumpsAdd(jumps, bsEmitJumpInst(e, BS_OP_DATA, 0, 0));
        return;
    }
    BSOperand cond = bsEmitExprConsumed(e, ast, expr);
    bsJumpsAdd(jumps, bsEmitJumpInst(e, jumpIfTrue ? BS_OP_JUMP_TRUE : BS_OP_JUMP_FALSE, cond, 0));
}


/* The conditional: if(cond, then, else) - the value of the branch taken, or null */
static void bsEmitIfTo(BSEmit *e, const BSAst *ast, uint32_t call, uint16_t dst)
{
    uint32_t cond = ast->nodes[call].a;
    if (cond == 0) {
        bsEmitArgOrNull(e, ast, 0, dst);
        return;
    }
    uint32_t then = ast->nodes[cond].next;
    uint32_t otherwise = then != 0 ? ast->nodes[then].next : 0;
    BSJumps jumpsElse;
    bsJumpsInit(&jumpsElse);
    bsEmitCondition(e, ast, cond, false, &jumpsElse);
    bsEmitArgOrNull(e, ast, then, dst);
    uint32_t jumpEnd = bsEmitJumpInst(e, BS_OP_JUMP, 0, 0);
    bsJumpsTarget(e, &jumpsElse, (uint32_t) e->count);
    bsEmitArgOrNull(e, ast, otherwise, dst);
    e->inst[jumpEnd].w = (uint32_t) e->count;
}


/* The intrinsic call opcode for a global call by name with "argCount" arguments, or CALL_NAME */
static uint8_t bsCallOpcode(const char *name, size_t argCount)
{
    static const struct {
        const char *name;
        uint8_t argMin;
        uint8_t argMax;
        uint8_t opcode;
    } table[] = {
        {"arrayGet", 2, 2, BS_OP_CALL_ARRAY_GET}, {"arrayLength", 1, 1, BS_OP_CALL_ARRAY_LENGTH},
        {"arrayPush", 2, 2, BS_OP_CALL_ARRAY_PUSH}, {"arraySet", 3, 3, BS_OP_CALL_ARRAY_SET},
        {"objectGet", 2, 3, BS_OP_CALL_OBJECT_GET}, {"objectHas", 2, 2, BS_OP_CALL_OBJECT_HAS},
        {"objectSet", 3, 3, BS_OP_CALL_OBJECT_SET}, {"stringCharCodeAt", 2, 2, BS_OP_CALL_STRING_CHAR_CODE_AT},
        {"stringLength", 1, 1, BS_OP_CALL_STRING_LENGTH}, {"stringSlice", 2, 3, BS_OP_CALL_STRING_SLICE},
        {"mathAbs", 1, 1, BS_OP_CALL_MATH}, {"mathCeil", 1, 1, BS_OP_CALL_MATH}, {"mathFloor", 1, 1, BS_OP_CALL_MATH},
        {"mathSign", 1, 1, BS_OP_CALL_MATH}, {"mathSqrt", 1, 1, BS_OP_CALL_MATH}
    };
    for (size_t ix = 0; ix < BS_COUNT_OF(table); ix++) {
        if (argCount >= table[ix].argMin && argCount <= table[ix].argMax && bsNameIs(name, table[ix].name)) {
            return table[ix].opcode;
        }
    }
    return BS_OP_CALL_NAME;
}


/* A call: the arguments are operands in the DATA words that follow the call instruction */
static void bsEmitCallTo(BSEmit *e, const BSAst *ast, uint32_t call, uint16_t dst)
{
    const BSNode *node = &ast->nodes[call];
    BSValue name = bsNodeText(ast, node);
    size_t argCount = node->b;
    bsEmitLimit(e, argCount, BS_OPERAND_MAX);
    uint16_t base = e->tempTop;
    BSOperand argsInline[BS_ARGS_INLINE];
    BSOperand *operands = argCount <= BS_ARGS_INLINE ? argsInline : bsAlloc(argCount * sizeof(BSOperand));
    size_t ix = 0;
    for (uint32_t arg = node->a; ix < argCount; arg = ast->nodes[arg].next) {
        operands[ix++] = bsEmitExprOperand(e, ast, arg);
    }
    e->tempTop = base;
    int slot = bsSlotFind(e, name);
    if (slot >= 0) {
        bsEmitInst(e, BS_OP_CALL_SLOT, dst, (uint16_t) slot, (uint16_t) argCount);
    } else {
        bsEmitInst(e, bsCallOpcode(bsStringData(name), argCount), dst, bsEmitSite(e, name), (uint16_t) argCount);
    }
    for (ix = 0; ix < argCount; ix += BS_OPERANDS_PER_DATA) {
        bsEmitInst(e, BS_OP_DATA, operands[ix],
                   ix + 1 < argCount ? operands[ix + 1] : 0,
                   ix + 2 < argCount ? operands[ix + 2] : 0);
    }
    if (operands != argsInline) {
        free(operands);
    }
}


/* Whether a call node is the conditional, if(cond, then, else) */
static bool bsNodeIsIf(const BSAst *ast, const BSNode *node)
{
    return bsStringIs(bsNodeText(ast, node), "if");
}


/* Compile an expression so its value lands in register "dst" */
static void bsEmitExprTo(BSEmit *e, const BSAst *ast, uint32_t id, uint16_t dst)
{
    const BSNode *node = &ast->nodes[id];
    switch (node->kind) {
    case BS_NODE_CALL: {
        if (!bsNodeIsIf(ast, node)) {
            bsEmitCallTo(e, ast, id, dst);
            return;
        }
        /*
         * A conditional or a short-circuit operator writes dst before its later operands are
         * evaluated, so when dst is a named local those operands might read the new value - they
         * accumulate in a temporary instead
         */
        uint16_t base;
        uint16_t acc = bsEmitAcc(e, dst, &base);
        bsEmitIfTo(e, ast, id, acc);
        bsEmitAccStore(e, dst, acc, base);
        return;
    }

    case BS_NODE_BINARY: {
        if (node->op == BS_NODE_AND || node->op == BS_NODE_OR) {
            uint16_t base;
            uint16_t acc = bsEmitAcc(e, dst, &base);
            bsEmitExprTo(e, ast, node->a, acc);
            uint32_t jump = bsEmitJumpInst(e, node->op == BS_NODE_AND ? BS_OP_JUMP_FALSE : BS_OP_JUMP_TRUE, acc, 0);
            bsEmitExprTo(e, ast, node->b, acc);
            e->inst[jump].w = (uint32_t) e->count;
            bsEmitAccStore(e, dst, acc, base);
            return;
        }
        /*
         * When dst is the topmost temporary, a left operand that needs one computes straight into
         * dst - the operator then reads and writes the same register, so a chain like a + b + c
         * appends to the string it is building in place. A left operand that is a constant or a
         * local allocates nothing, and dst is taken back before the right operand is compiled.
         */
        uint16_t base = e->tempTop;
        bool dstTop = e->tempTop != 0 && (size_t) dst + 1 == e->slotCount + e->tempTop;
        if (dstTop) {
            e->tempTop--;
        }
        BSOperand left = bsEmitExprOperand(e, ast, node->a);
        if (dstTop && e->tempTop < base) {
            e->tempTop = base;
        }
        BSOperand right = bsEmitExprOperand(e, ast, node->b);
        e->tempTop = base;
        bsEmitInst(e, (uint8_t) node->op, dst, left, right);
        return;
    }

    case BS_NODE_UNARY:
        bsEmitInst(e, (uint8_t) node->op, dst, bsEmitExprConsumed(e, ast, node->a), 0);
        return;

    case BS_NODE_GROUP:
        bsEmitExprTo(e, ast, node->a, dst);
        return;

    case BS_NODE_VARIABLE: {
        /* A global variable or an unassigned local loads into dst; a constant or an assigned local moves */
        BSValue name = bsNodeText(ast, node);
        const char *text = bsStringData(name);
        if (!bsNameIs(text, "null") && !bsNameIs(text, "true") && !bsNameIs(text, "false")) {
            int slot = bsSlotFind(e, name);
            if (slot < 0) {
                bsEmitInst(e, BS_OP_LOAD_NAME, dst, bsEmitSite(e, name), 0);
                return;
            }
            if (!bsAssignedTest(e, slot)) {
                bsEmitInst(e, BS_OP_LOAD_SLOT, dst, (uint16_t) slot, 0);
                return;
            }
        }
        break;
    }

    default:
        /* A number or a string */
        break;
    }
    BSOperand operand = bsEmitExprOperand(e, ast, id);
    if (operand != dst) {
        bsEmitInst(e, BS_OP_MOVE, dst, operand, 0);
    }
}


/* Compile an expression statement - a call drops its result; anything else is computed and left */
static void bsEmitExprDiscard(BSEmit *e, const BSAst *ast, uint32_t id)
{
    const BSNode *node = &ast->nodes[id];
    if (node->kind == BS_NODE_CALL && !bsNodeIsIf(ast, node)) {
        bsEmitCallTo(e, ast, id, BS_REG_DISCARD);
        return;
    }
    bsEmitExprConsumed(e, ast, id);
}


int bsCoverLine(const uint32_t *pcs, const int *lines, size_t count, size_t pc)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (pcs[middle] <= pc) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low == 0 ? 0 : lines[low - 1];
}


/* Record a statement's model and line */
static void bsEmitCover(BSEmit *e, const BSNode *node)
{
    if (e->coverCount == e->coverCap) {
        e->coverCap = e->coverCap != 0 ? e->coverCap * 2 : 8;
        e->cover = bsRealloc(e->cover, e->coverCap * sizeof(BSValue));
        e->coverLines = bsRealloc(e->coverLines, e->coverCap * sizeof(int));
        e->coverPcs = bsRealloc(e->coverPcs, e->coverCap * sizeof(uint32_t));
    }
    e->cover[e->coverCount] = node->model;
    e->coverLines[e->coverCount] = node->line;
    e->coverPcs[e->coverCount] = (uint32_t) e->count;
    if (!e->script->system) {
        /* A system script is never statement-counted or coverage-recorded, so it has no markers */
        bsEmitInst(e, BS_OP_STMT, (uint16_t) e->coverCount, 0, 0);
    }
    e->coverCount++;
}


static void bsEmitFunction(BSEmit *e, const BSAst *ast, uint32_t id);


/* Emit one statement node */
static void bsEmitStatement(BSEmit *e, const BSAst *ast, uint32_t id)
{
    const BSNode *node = &ast->nodes[id];
    bsEmitCover(e, node);

    switch (node->kind) {
    case BS_NODE_EXPR: {
        if (node->text == 0) {
            bsEmitExprDiscard(e, ast, node->a);
            return;
        }
        BSValue name = bsNodeText(ast, node);
        int slot = bsSlotFind(e, name);
        if (slot >= 0) {
            bsEmitExprTo(e, ast, node->a, (uint16_t) slot);
            bsAssignedSet(e, slot);
            return;
        }
        uint16_t site = bsEmitSite(e, name);
        bsEmitInst(e, BS_OP_STORE_NAME, site, bsEmitExprConsumed(e, ast, node->a), 0);
        return;
    }

    case BS_NODE_JUMP: {
        BSValue label = bsNodeText(ast, node);
        if (node->a == 0) {
            bsEmitJumpLabel(e, bsEmitJumpInst(e, BS_OP_JUMP, 0, 0), label);
            return;
        }
        BSJumps jumps;
        bsJumpsInit(&jumps);
        bsEmitCondition(e, ast, node->a, true, &jumps);
        bsJumpsLabel(e, &jumps, label);
        return;
    }

    case BS_NODE_RETURN:
        bsEmitInst(e, BS_OP_RETURN, node->a != 0 ? bsEmitExprConsumed(e, ast, node->a) : BS_OPERAND_NULL, 0, 0);
        return;

    case BS_NODE_LABEL:
        bsEmitLabel(e, bsNodeText(ast, node));
        return;

    case BS_NODE_FUNCTION:
        bsEmitFunction(e, ast, id);
        return;

    case BS_NODE_INCLUDE: {
        /* One instruction runs the statement's includes, which fetch together */
        size_t first = e->includeCount;
        for (uint32_t item = node->a; item != 0; item = ast->nodes[item].next) {
            bsEmitLimit(e, e->includeCount, BS_OPERAND_MAX);
            BS_GROW(e->includes, e->includeCount, e->includeCap, 4);
            e->includes[e->includeCount].url = bsRetain(bsNodeText(ast, &ast->nodes[item]));
            e->includes[e->includeCount].system = ast->nodes[item].flag;
            e->includeCount++;
        }
        bsEmitInst(e, BS_OP_INCLUDE, (uint16_t) first, (uint16_t) (e->includeCount - first), 0);
        return;
    }
    }
}


static void bsEmitStatements(BSEmit *e, const BSAst *ast, const uint32_t *statements, size_t count)
{
    for (size_t ix = 0; ix < count; ix++) {
        if (e->assignedWords != 0 && (ix == 0 || e->blockOf[ix] != e->blockOf[ix - 1])) {
            memcpy(e->assigned, &e->blockIn[e->blockOf[ix] * e->assignedWords],
                   e->assignedWords * sizeof(uint32_t));
        }
        bsEmitStatement(e, ast, statements[ix]);
    }
}


/*
 * A constant operand names its register: the constants follow the slots and temporaries in a
 * chunk's registers, so once the temporaries are counted every operand that named a constant
 * becomes that register, and the interpreter reads every operand the same way
 */
static inline uint16_t bsOperandRelocate(uint16_t operand, uint16_t base)
{
    return (operand & BS_OPERAND_CONST) != 0 ? (uint16_t) (base + BS_OPERAND_INDEX(operand)) : operand;
}

static void bsCodeRelocateConstants(BSInst *inst, size_t count, uint16_t base)
{
    for (size_t pc = 0; pc < count; pc++) {
        uint8_t op = inst[pc].op;
        if (op == BS_OP_JUMP_FALSE || op == BS_OP_JUMP_TRUE || op == BS_OP_RETURN) {
            inst[pc].a = bsOperandRelocate(inst[pc].a, base);
        } else if (op == BS_OP_MOVE || op == BS_OP_STORE_NAME || op == BS_OP_NEG || op == BS_OP_NOT || op == BS_OP_BNOT) {
            inst[pc].b = bsOperandRelocate(inst[pc].b, base);
        } else if (op >= BS_OP_ADD && op <= BS_OP_SHR) {
            inst[pc].b = bsOperandRelocate(inst[pc].b, base);
            inst[pc].c = bsOperandRelocate(inst[pc].c, base);
        } else if (op >= BS_OP_JUMP_EQ && op <= BS_OP_JUMP_GE) {
            /* The target word that follows is not operands */
            inst[pc].b = bsOperandRelocate(inst[pc].b, base);
            inst[pc].c = bsOperandRelocate(inst[pc].c, base);
            pc++;
        } else if (op == BS_OP_JUMP_UNDEF) {
            /* The line word that follows is not operands, and a is the label's name index */
            pc++;
        } else if (op == BS_OP_CALL_NAME || op == BS_OP_CALL_SLOT ||
                   (op >= BS_OP_CALL_ARRAY_GET && op <= BS_OP_CALL_MATH)) {
            /* The argument operands, three per data word - an unused field is zero, which names a slot */
            size_t argCount = inst[pc].c;
            for (size_t ix = 0; ix < argCount; ix += BS_OPERANDS_PER_DATA) {
                pc++;
                inst[pc].a = bsOperandRelocate(inst[pc].a, base);
                inst[pc].b = bsOperandRelocate(inst[pc].b, base);
                inst[pc].c = bsOperandRelocate(inst[pc].c, base);
            }
        }
    }
}


/*
 * Finish a chunk into "code", resolving its forward jumps and naming its constants' registers.
 * Returns false if an operand space overflowed - the chunk is then invalid, but complete, so
 * bsCodeFree releases it.
 */
static bool bsEmitFinish(BSEmit *e, BSCode *code)
{
    for (size_t ix = 0; ix < e->patchCount; ix++) {
        BSValue pc = bsObjectGetString(e->labels, e->patches[ix].label);
        if (pc.type == BS_NUMBER) {
            e->inst[e->patches[ix].pc].w = (uint32_t) pc.u.number;
        } else {
            /*
             * A jump to a missing label only errors if the jump is taken. The trap sits past the
             * chunk's return, so it carries the jump statement's line in a data word.
             */
            uint16_t name = bsEmitName(e, e->patches[ix].label);
            int line = bsCoverLine(e->coverPcs, e->coverLines, e->coverCount, e->patches[ix].pc);
            uint32_t trap = bsEmitInst(e, BS_OP_JUMP_UNDEF, name, 0, 0);
            bsEmitJumpInst(e, BS_OP_DATA, 0, (uint32_t) line);
            e->inst[e->patches[ix].pc].w = trap;
        }
        bsRelease(e->patches[ix].label);
    }
    bsCodeRelocateConstants(e->inst, e->count, (uint16_t) (e->slotCount + e->tempMax));
    free(e->patches);
    free(e->assigned);
    free(e->blockOf);
    free(e->blockIn);
    bsRelease(e->labels);
    bsRelease(e->slotMap);
    bsRelease(e->constMap);
    bsRelease(e->nameMap);

    memset(code, 0, sizeof(*code));
    code->inst = e->inst;
    code->count = e->count;
    code->tempCount = e->tempMax;
    code->constants = e->constants;
    code->constantCount = e->constCount;
    code->names = e->names;
    code->nameCount = e->nameCount;
    code->includes = e->includes;
    code->includeCount = e->includeCount;
    code->cover = e->cover;
    code->coverLines = e->coverLines;
    code->coverPcs = e->coverPcs;
    code->coverCount = e->coverCount;
    code->slotNames = e->slotNames;
    code->slotCount = e->slotCount;
    code->caches = e->caches;
    return !e->overflow;
}


/*
 * End a chunk whose statements were loaded with result "loaded": return "ret" and finish it into
 * "code". Returns false - the chunk released - for a malformed statement or an overflowed operand
 * space.
 */
static bool bsEmitEnd(BSEmit *e, bool loaded, BSCode *code, BSOperand ret)
{
    bsEmitInst(e, BS_OP_RETURN, ret, 0, 0);
    bool finished = bsEmitFinish(e, code);
    if (!loaded || !finished) {
        bsCodeFree(code);
        return false;
    }
    return true;
}


static void bsEmitFunction(BSEmit *e, const BSAst *ast, uint32_t id)
{
    const BSNode *node = &ast->nodes[id];
    BSFunctionDef *def = bsAlloc(sizeof(BSFunctionDef));
    memset(def, 0, sizeof(*def));
    def->name = bsRetain(bsNodeText(ast, node));
    def->lastArgArray = node->flag;
    BS_GROW(e->script->functions, e->script->functionCount, *e->functionCap, 8);
    uint32_t index = (uint32_t) e->script->functionCount;
    e->script->functions[e->script->functionCount++] = def;

    BSEmit body;
    bsEmitInit(&body, e->script, e->functionCap);
    for (uint32_t arg = node->b; arg != 0; arg = ast->nodes[arg].next) {
        bsSlotAdd(&body, bsNodeText(ast, &ast->nodes[arg]), true);
    }
    def->argCount = body.slotCount;

    /* The body's statements, indexed for the definite-assignment analysis */
    uint32_t *statements = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (uint32_t statement = node->a; statement != 0; statement = ast->nodes[statement].next) {
        BS_GROW(statements, count, capacity, 16);
        statements[count++] = statement;
    }
    for (size_t ix = 0; ix < count; ix++) {
        BSValue assign = bsStatementAssignName(ast, statements[ix]);
        if (assign.type == BS_STRING) {
            bsSlotAdd(&body, assign, false);
        }
    }
    bsAssignedAnalyze(&body, ast, statements, count, def->argCount);
    bsEmitStatements(&body, ast, statements, count);
    free(statements);
    bsEmitLimit(e, index, BS_INDEX_MAX);
    if (!bsEmitEnd(&body, true, &def->code, BS_OPERAND_NULL)) {
        /* The body overflowed an operand space, so the script is invalid */
        e->overflow = true;
        return;
    }
    bsEmitInst(e, BS_OP_FUNCTION, (uint16_t) index, 0, 0);
}


BSExpr *bsExprFromModel(BSValue model)
{
    bsModelKeysInit();
    BSAst ast;
    bsAstInit(&ast);
    uint32_t node = bsAstExpr(&ast, model);
    BSEmit e;
    bsEmitInit(&e, NULL, NULL);
    BSOperand operand = node != 0 ? bsEmitExprOperand(&e, &ast, node) : BS_OPERAND_NULL;
    bsAstFree(&ast);
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(*expr));
    if (!bsEmitEnd(&e, node != 0, &expr->code, operand)) {
        bsExprFree(expr);
        return NULL;
    }
    expr->model = bsRetain(model);
    return expr;
}


/* A new script with no name and no model, keeping "lines" - its source lines array, or none */
static BSScript *bsScriptNew(BSValue lines)
{
    BSScript *script = bsAlloc(sizeof(BSScript));
    memset(script, 0, sizeof(*script));
    script->refcount = 1;
    script->startLineNumber = 1;
    script->scriptLines = lines.type == BS_ARRAY ? bsRetain(lines) : bsArrayNew();
    return script;
}


/* Take a script's name and system flag from its model's members; "scriptName" overrides the name */
static void bsScriptInfo(BSScript *script, BSValue model, const char *scriptName)
{
    script->system = bsValueBoolean(bsObjectGetString(model, bsKeys.system));
    BSValue modelName = bsObjectGetString(model, bsKeys.scriptName);
    if (scriptName != NULL) {
        script->scriptName = bsStringNew(scriptName);
    } else if (modelName.type == BS_STRING) {
        script->scriptName = bsRetain(modelName);
    }
}


BSScript *bsScriptFromModel(BSValue model, const char *scriptName)
{
    if (model.type != BS_OBJECT) {
        return NULL;
    }
    bsModelKeysInit();
    BSValue statements = bsObjectGetString(model, bsKeys.statements);
    if (statements.type != BS_ARRAY) {
        return NULL;
    }

    BSScript *script = bsScriptNew(bsObjectGetString(model, bsKeys.scriptLines));
    script->model = bsRetain(model);
    bsScriptInfo(script, model, scriptName);

    /* Each statement is loaded, emitted, and dropped from the arena before the next */
    size_t functionCap = 0;
    BSEmit e;
    bsEmitInit(&e, script, &functionCap);
    BSAst ast;
    bsAstInit(&ast);
    bool loaded = true;
    size_t count = bsArrayCount(statements);
    for (size_t ix = 0; loaded && ix < count; ix++) {
        bsAstReset(&ast);
        uint32_t node = bsAstStatement(&ast, bsArrayGet(statements, ix));
        loaded = node != 0;
        if (loaded) {
            bsEmitStatement(&e, &ast, node);
        }
    }
    bsAstFree(&ast);
    if (!bsEmitEnd(&e, loaded, &script->code, BS_OPERAND_NULL)) {
        bsScriptRelease(script);
        return NULL;
    }
    return script;
}


BSScript *bsScriptFromModelJSON(const char *text, size_t size, const char *scriptName, const char **error)
{
    const char *jsonError = NULL;
    BSValue model = bsJSONDecode(text, size, &jsonError);
    BSScript *script = jsonError == NULL ? bsScriptFromModel(model, scriptName) : NULL;
    bsRelease(model);
    if (error != NULL) {
        *error = jsonError != NULL ? jsonError : script == NULL ? "Invalid BareScript model" : NULL;
    }
    if (script != NULL) {
        bsScriptForgetModel(script);
    }
    return script;
}


/*
 * The binary model
 *
 * The bundled include library's encoding, which bin/includeSource.bare writes: a version byte, 1;
 * the string table - a count, then each string as a length and its UTF-8 bytes; the statements -
 * a count, then each. Counts, lengths, indexes, and line numbers are unsigned LEB128 varints; a
 * string is referred to by its table index plus one, zero meaning none. A statement is a kind byte
 * (1 expr, 2 jump, 3 return, 4 label, 5 function, 6 include), its line number, then its members:
 * expr - the name assigned or none, the expression; jump - the label, the condition or none;
 * return - the expression or none; label - the name; function - the name, a flags byte (1: the
 * last argument takes the rest), the argument count and names, the statement count and
 * statements; include - the count, then each include's url and a system flag byte. An expression
 * is a tag byte - 0 none, 1 an integer as a zigzag varint, 2 a number as its text, 3 a string,
 * 4 a variable, 5 a call (the name, the argument count, the arguments), 6 a binary operator (the
 * operator, left, right), 7 a unary operator (the operator, the operand), 8 a group (the
 * expression). The table's strings are interned once, so a name costs one reference per use.
 * A read past the end, an unknown byte, or a reference past the table fails the whole model.
 */

/*
 * A child is read into a local before it is stored in its parent node: the read may grow the arena,
 * and C does not order an assignment's two sides
 */

#define BS_MODEL_DEPTH_MAX 1000

typedef struct BSModelReader {
    const unsigned char *data;
    size_t size;
    size_t offset;
    BSAst *ast;
    BSValue *strings;
    size_t stringCount;
    bool shared;         /* version 2: a reference's low bit says shared table, the rest the index */
    bool failed;
} BSModelReader;


static unsigned bsModelByte(BSModelReader *reader)
{
    if (reader->offset >= reader->size) {
        reader->failed = true;
        return 0;
    }
    return reader->data[reader->offset++];
}


static uint64_t bsModelVarint(BSModelReader *reader)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        unsigned byte = bsModelByte(reader);
        value |= (uint64_t) (byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    reader->failed = true;
    return 0;
}


/* A string reference's string, borrowed - a null value for none; failed for a reference past the table */
static BSValue bsModelString(BSModelReader *reader)
{
    uint64_t index = bsModelVarint(reader);
    if (index == 0) {
        return bsNull();
    }
    if (reader->shared) {
        BSValue string;
        if ((index & 1) == 0) {
            if (bsIncludeSharedString((size_t) ((index - 2) >> 1), &string)) {
                return string;
            }
            reader->failed = true;
            return bsNull();
        }
        index = (index + 1) >> 1;
    }
    if (index > reader->stringCount) {
        reader->failed = true;
        return bsNull();
    }
    return reader->strings[index - 1];
}


/* A required string's arena index, or zero */
static uint32_t bsModelText(BSModelReader *reader)
{
    BSValue string = bsModelString(reader);
    if (string.type != BS_STRING) {
        reader->failed = true;
        return 0;
    }
    return bsAstString(reader->ast, bsRetain(string));
}


/* An expression, or zero - for none where one is not required, or on failure */
static uint32_t bsModelExpr(BSModelReader *reader, int depth, bool required)
{
    BSAst *ast = reader->ast;
    if (depth >= BS_MODEL_DEPTH_MAX) {
        reader->failed = true;
        return 0;
    }
    unsigned tag = bsModelByte(reader);
    uint32_t node;
    switch (tag) {
    case 0:
        if (required) {
            reader->failed = true;
        }
        return 0;
    case 1: {
        uint64_t zigzag = bsModelVarint(reader);
        double number = (double) (zigzag >> 1);
        node = bsAstNode(ast, BS_NODE_NUMBER);
        ast->nodes[node].number = (zigzag & 1) != 0 ? -number - 1 : number;
        return node;
    }
    case 2: {
        BSValue text = bsModelString(reader);
        double number;
        if (text.type != BS_STRING || !bsNumberParse(bsStringSpan(text), bsStringSize(text), &number)) {
            reader->failed = true;
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_NUMBER);
        ast->nodes[node].number = number;
        return node;
    }
    case 3:
    case 4: {
        node = bsAstNode(ast, tag == 3 ? BS_NODE_STRING : BS_NODE_VARIABLE);
        ast->nodes[node].text = bsModelText(reader);
        return node;
    }
    case 5: {
        node = bsAstNode(ast, BS_NODE_CALL);
        ast->nodes[node].text = bsModelText(reader);
        uint64_t count = bsModelVarint(reader);
        ast->nodes[node].b = (uint32_t) count;
        uint32_t tail = 0;
        for (uint64_t ix = 0; ix < count && !reader->failed; ix++) {
            uint32_t arg = bsModelExpr(reader, depth + 1, true);
            bsAstAppend(ast, &ast->nodes[node].a, &tail, arg);
        }
        return node;
    }
    case 6:
    case 7: {
        BSValue op = bsModelString(reader);
        uint16_t nodeOp = 0;
        if (op.type == BS_STRING) {
            nodeOp = tag == 6 ? bsBinaryNodeOp(bsStringData(op)) : bsUnaryOpcode(bsStringData(op));
        }
        if (nodeOp == 0) {
            reader->failed = true;
            return 0;
        }
        node = bsAstNode(ast, tag == 6 ? BS_NODE_BINARY : BS_NODE_UNARY);
        ast->nodes[node].op = nodeOp;
        uint32_t left = bsModelExpr(reader, depth + 1, true);
        ast->nodes[node].a = left;
        if (tag == 6) {
            uint32_t right = bsModelExpr(reader, depth + 1, true);
            ast->nodes[node].b = right;
        }
        return node;
    }
    case 8: {
        node = bsAstNode(ast, BS_NODE_GROUP);
        uint32_t sub = bsModelExpr(reader, depth + 1, true);
        ast->nodes[node].a = sub;
        return node;
    }
    default:
        reader->failed = true;
        return 0;
    }
}


/* A statement, or zero on failure */
static uint32_t bsModelStatement(BSModelReader *reader, int depth)
{
    BSAst *ast = reader->ast;
    if (depth >= BS_MODEL_DEPTH_MAX) {
        reader->failed = true;
        return 0;
    }
    unsigned kind = bsModelByte(reader);
    uint64_t line = bsModelVarint(reader);
    uint32_t node;
    uint32_t tail = 0;
    switch (kind) {
    case 1: {
        node = bsAstNode(ast, BS_NODE_EXPR);
        BSValue name = bsModelString(reader);
        if (name.type == BS_STRING) {
            ast->nodes[node].text = bsAstString(ast, bsRetain(name));
        }
        uint32_t expr = bsModelExpr(reader, depth + 1, true);
        ast->nodes[node].a = expr;
        break;
    }
    case 2: {
        node = bsAstNode(ast, BS_NODE_JUMP);
        ast->nodes[node].text = bsModelText(reader);
        uint32_t cond = bsModelExpr(reader, depth + 1, false);
        ast->nodes[node].a = cond;
        break;
    }
    case 3: {
        node = bsAstNode(ast, BS_NODE_RETURN);
        uint32_t expr = bsModelExpr(reader, depth + 1, false);
        ast->nodes[node].a = expr;
        break;
    }
    case 4: {
        node = bsAstNode(ast, BS_NODE_LABEL);
        ast->nodes[node].text = bsModelText(reader);
        break;
    }
    case 5: {
        node = bsAstNode(ast, BS_NODE_FUNCTION);
        ast->nodes[node].text = bsModelText(reader);
        ast->nodes[node].flag = (uint8_t) (bsModelByte(reader) & 1);
        uint64_t argCount = bsModelVarint(reader);
        for (uint64_t ix = 0; ix < argCount && !reader->failed; ix++) {
            uint32_t arg = bsAstNode(ast, BS_NODE_ARG);
            ast->nodes[arg].text = bsModelText(reader);
            bsAstAppend(ast, &ast->nodes[node].b, &tail, arg);
        }
        tail = 0;
        uint64_t count = bsModelVarint(reader);
        for (uint64_t ix = 0; ix < count && !reader->failed; ix++) {
            uint32_t statement = bsModelStatement(reader, depth + 1);
            bsAstAppend(ast, &ast->nodes[node].a, &tail, statement);
        }
        break;
    }
    case 6: {
        node = bsAstNode(ast, BS_NODE_INCLUDE);
        uint64_t count = bsModelVarint(reader);
        if (count == 0) {
            reader->failed = true;
        }
        for (uint64_t ix = 0; ix < count && !reader->failed; ix++) {
            uint32_t item = bsAstNode(ast, BS_NODE_INCLUDE_ITEM);
            ast->nodes[item].text = bsModelText(reader);
            ast->nodes[item].flag = (uint8_t) (bsModelByte(reader) & 1);
            bsAstAppend(ast, &ast->nodes[node].a, &tail, item);
        }
        break;
    }
    default:
        reader->failed = true;
        return 0;
    }
    if (reader->failed || line > INT32_MAX) {
        return 0;
    }
    ast->nodes[node].line = (int32_t) line;
    return node;
}


BSScript *bsScriptFromModelBinary(const unsigned char *data, size_t size, const char *scriptName)
{
    bsModelKeysInit();
    BSModelReader reader = {.data = data, .size = size};
    unsigned version = bsModelByte(&reader);
    reader.shared = version == 2;

    /* The string table, interned - each string takes at least a byte, which bounds the count */
    uint64_t stringCount = bsModelVarint(&reader);
    if ((version != 1 && version != 2) || stringCount > size) {
        reader.failed = true;
    }
    if (!reader.failed) {
        reader.strings = bsAlloc((size_t) (stringCount + 1) * sizeof(BSValue));
        for (uint64_t ix = 0; ix < stringCount && !reader.failed; ix++) {
            uint64_t length = bsModelVarint(&reader);
            if (length > size - reader.offset) {
                reader.failed = true;
                break;
            }
            reader.strings[reader.stringCount++] = bsStringIntern((const char *) data + reader.offset, (size_t) length);
            reader.offset += (size_t) length;
        }
    }
    bool decoded = !reader.failed;

    /* Each statement is read, emitted, and dropped from the arena before the next - with no
       statement markers, the binary models being the bundled library's, which is system code */
    BSScript *script = bsScriptNew(bsNull());
    script->system = true;
    size_t functionCap = 0;
    BSEmit e;
    bsEmitInit(&e, script, &functionCap);
    BSAst ast;
    bsAstInit(&ast);
    reader.ast = &ast;
    uint64_t statementCount = bsModelVarint(&reader);
    for (uint64_t ix = 0; ix < statementCount && decoded; ix++) {
        bsAstReset(&ast);
        uint32_t node = bsModelStatement(&reader, 1);
        decoded = node != 0;
        if (decoded) {
            bsEmitStatement(&e, &ast, node);
        }
    }
    decoded = decoded && !reader.failed && reader.offset == size;
    bsAstFree(&ast);
    for (size_t ix = 0; ix < reader.stringCount; ix++) {
        bsRelease(reader.strings[ix]);
    }
    free(reader.strings);
    if (!bsEmitEnd(&e, decoded, &script->code, BS_OPERAND_NULL)) {
        bsScriptRelease(script);
        return NULL;
    }
    script->scriptName = bsStringNew(scriptName);
    return script;
}


void bsScriptForgetModel(BSScript *script)
{
    bsRelease(script->model);
    script->model = bsNull();

    /* The chunks' statement models are borrowed from the model - bsScriptRestoreCover brings them back */
    free(script->code.cover);
    script->code.cover = NULL;
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        free(script->functions[ix]->code.cover);
        script->functions[ix]->code.cover = NULL;
    }
}


/* Move a chunk's statement models from a freshly compiled twin of the same model - unless the
   script's lines no longer parse to it */
static bool bsCodeTakeCover(BSCode *code, BSCode *twin)
{
    if (twin->coverCount != code->coverCount) {
        return false;
    }
    code->cover = twin->cover;
    twin->cover = NULL;
    return true;
}


bool bsScriptRestoreCover(BSScript *script)
{
    /* Parse the lines again and compile the model once more - its chunks mirror this script's */
    BSValue model = bsScriptReparse(script);
    BSScript *twin = model.type == BS_OBJECT ? bsScriptFromModel(model, NULL) : NULL;
    bool restored = twin != NULL && twin->functionCount == script->functionCount &&
        bsCodeTakeCover(&script->code, &twin->code);
    for (size_t ix = 0; restored && ix < script->functionCount; ix++) {
        restored = bsCodeTakeCover(&script->functions[ix]->code, &twin->functions[ix]->code);
    }
    if (restored) {
        bsRelease(script->model);
        script->model = bsRetain(model);
    }
    if (twin != NULL) {
        bsScriptRelease(twin);
    }
    bsRelease(model);
    return restored;
}


BSValue bsExprToModel(const BSExpr *expr)
{
    return bsRetain(expr->model);
}


BSValue bsScriptToModel(const BSScript *script)
{
    /* A parsed script keeps its lines, not its model - parse them again */
    BSValue source = script->model.type == BS_OBJECT ? bsRetain(script->model) : bsScriptReparse(script);
    BSValue statements = bsObjectGetString(source, bsKeys.statements);
    BSValue model = bsObjectNew();
    bsObjectSetString(model, bsKeys.statements, statements.type == BS_ARRAY ? bsRetain(statements) : bsArrayNew());
    bsRelease(source);
    if (script->scriptName.type == BS_STRING) {
        bsObjectSetString(model, bsKeys.scriptName, bsRetain(script->scriptName));
    }
    if (bsArrayCount(script->scriptLines) != 0) {
        bsObjectSetString(model, bsKeys.scriptLines, bsRetain(script->scriptLines));
    }
    if (script->system) {
        bsObjectSetString(model, bsKeys.system, bsBoolean(true));
    }
    return model;
}
