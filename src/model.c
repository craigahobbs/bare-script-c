/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript model conversion
 *
 * The parser produces a JSON BareScript model. This file compiles that model to bytecode and
 * keeps the original model on the script for lint and coverage. Jump labels become instruction
 * indexes and function-local names become slot indexes during emit - there is no expression tree.
 */

#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"
#include "barescript/parser.h"
#include "barescript/runtime.h"

#include "internal.h"


/*
 * Finish a cached system include's chunk
 *
 * A system script is never statement-counted or coverage-recorded, so its STMT markers only cost
 * dispatch: they are stripped, with jump targets remapped and each statement's start index kept
 * in coverPcs for error line numbers. The borrowed statement models are dropped with the parser
 * model. Data words (an argument count, a trap's line) are never STMT and never jumps.
 */
static void bsCodeFinishSystem(BSCode *code)
{
    free(code->cover);
    code->cover = NULL;

    size_t count = code->count;
    uint32_t *map = bsAlloc(count * sizeof(uint32_t));
    uint32_t *inst = bsAlloc(count * sizeof(uint32_t));
    size_t stripped = 0;
    for (size_t pc = 0; pc < count; pc++) {
        uint32_t word = code->inst[pc];
        map[pc] = (uint32_t) stripped;
        if (BS_OP(word) == BS_OP_STMT) {
            code->coverPcs[BS_ARG(word)] = (uint32_t) stripped;
        } else {
            inst[stripped++] = word;
        }
    }
    for (size_t pc = 0; pc < stripped; pc++) {
        uint8_t op = BS_OP(inst[pc]);
        if (op == BS_OP_JUMP || op == BS_OP_JUMP_FALSE || op == BS_OP_JUMP_TRUE) {
            inst[pc] = BS_INST(op, map[BS_ARG(inst[pc])]);
        }
    }
    free(map);
    free(code->inst);
    code->inst = inst;
    code->count = stripped;
}


void bsScriptDropModel(BSScript *script)
{
    bsCodeFinishSystem(&script->code);
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        bsCodeFinishSystem(&script->functions[ix]->code);
    }
    bsRelease(script->model);
    script->model = bsNull();
}


void bsScriptForgetModel(BSScript *script)
{
    bsRelease(script->model);
    script->model = bsNull();
}


static void bsCodeFree(BSCode *code)
{
    free(code->inst);
    for (size_t ix = 0; ix < code->constantCount; ix++) {
        bsRelease(code->constants[ix]);
    }
    free(code->constants);
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
    for (size_t ix = 0; ix < def->argCount; ix++) {
        bsRelease(def->argNames[ix]);
    }
    free(def->argNames);
    bsCodeFree(&def->code);
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
 * Emit
 */


/*
 * The model keys, interned once so every lookup compares pointers. The JSON decoder and the
 * parser script produce interned keys, so a model object's keys are never compared by content.
 */
static struct {
    BSValue args, binary, expr, function, group, include, includes, jump, label, lastArgArray, left, lineNumber, name, number, op, return_, right, scriptLines, scriptName, statements, string, system, unary, url, variable;
} bsKeys;
static bool bsKeysReady;


void bsModelKeysInit(void)
{
    if (bsKeysReady) {
        return;
    }
#define BS_KEY(field, text) bsKeys.field = bsStringIntern(text, sizeof(text) - 1)
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
    bsRelease(bsStringIntern("async", sizeof("async") - 1));
    bsRelease(bsStringIntern("lineCount", sizeof("lineCount") - 1));
    bsKeysReady = true;
}


/* Retain an interned name; intern an ordinary one. Model strings are interned on decode. */
static BSValue bsInternName(BSValue name)
{
    if ((name.u.string->flags & BS_STR_INTERNED) != 0) {
        return bsRetain(name);
    }
    return bsStringIntern(bsStringData(name), bsStringSize(name));
}




typedef struct {
    size_t pc;
    BSValue label;
} BSPatch;


typedef struct {
    uint32_t *inst;
    size_t count;
    size_t cap;
    BSValue *constants;
    size_t constCount;
    size_t constCap;
    BSValue *cover;
    int *coverLines;
    uint32_t *coverPcs;
    size_t coverCount;
    size_t coverCap;
    BSInclude *includes;
    size_t includeCount;
    size_t includeCap;
    uint32_t *callNames;  /* per CALL_NAME or LOAD_NAME site, the name's constant index */
    size_t callCount;
    size_t callCap;
    int depth;            /* the value stack depth after the last emitted instruction */
    int maxDepth;
    size_t targetAt;      /* the instruction index a jump was most recently patched to target; zero before any */
    BSValue *slotNames;
    size_t slotCount;
    size_t slotCap;
    BSValue slotMap;
    BSValue constMap;     /* interned constant string -> constant index */
    BSValue labels;
    BSPatch *patches;
    size_t patchCount;
    size_t patchCap;
    BSScript *script;
    size_t *functionCap;
} BSEmit;


/* An instruction's net effect on the value stack depth. A call's effect lands on its ARGC word. */
static int bsOpStackEffect(uint8_t op, uint32_t arg)
{
    switch (op) {
    case BS_OP_LOAD_NULL:
    case BS_OP_LOAD_TRUE:
    case BS_OP_LOAD_FALSE:
    case BS_OP_LOAD_CONST:
    case BS_OP_LOAD_SLOT:
    case BS_OP_LOAD_NAME:
    case BS_OP_DUP:
        return 1;
    case BS_OP_STORE_SLOT:
    case BS_OP_STORE_NAME:
    case BS_OP_POP:
    case BS_OP_JUMP_FALSE:
    case BS_OP_JUMP_TRUE:
    case BS_OP_RETURN:
    case BS_OP_ADD:
    case BS_OP_SUB:
    case BS_OP_MUL:
    case BS_OP_DIV:
    case BS_OP_MOD:
    case BS_OP_POW:
    case BS_OP_EQ:
    case BS_OP_NE:
    case BS_OP_LT:
    case BS_OP_LE:
    case BS_OP_GT:
    case BS_OP_GE:
    case BS_OP_BAND:
    case BS_OP_BOR:
    case BS_OP_BXOR:
    case BS_OP_SHL:
    case BS_OP_SHR:
        return -1;
    case BS_OP_ARGC:
        return 1 - (int) arg;
    default:
        return 0;
    }
}


/* Append a code word without stack accounting - for data words the interpreter never dispatches */
static uint32_t bsEmitWord(BSEmit *e, uint8_t op, uint32_t arg)
{
    if (e->count == e->cap) {
        e->cap = e->cap != 0 ? e->cap * 2 : 32;
        e->inst = bsRealloc(e->inst, e->cap * sizeof(uint32_t));
    }
    uint32_t pc = (uint32_t) e->count;
    e->inst[e->count++] = BS_INST(op, arg);
    return pc;
}


static uint32_t bsEmitInst(BSEmit *e, uint8_t op, uint32_t arg)
{
    uint32_t pc = bsEmitWord(e, op, arg);
    e->depth += bsOpStackEffect(op, arg);
    if (e->depth > e->maxDepth) {
        e->maxDepth = e->depth;
    }
    return pc;
}


static uint32_t bsEmitConst(BSEmit *e, BSValue value)
{
    /* Interned strings - names and literals - are shared through a map, so a chunk holds each once */
    bool interned = value.type == BS_STRING && (value.u.string->flags & BS_STR_INTERNED) != 0;
    if (interned) {
        if (e->constMap.type != BS_OBJECT) {
            e->constMap = bsObjectNew();
        }
        BSValue index = bsObjectGetString(e->constMap, value);
        if (index.type == BS_NUMBER) {
            return (uint32_t) index.u.number;
        }
        bsObjectSetString(e->constMap, value, bsNumber((double) e->constCount));
    }
    if (e->constCount == e->constCap) {
        e->constCap = e->constCap != 0 ? e->constCap * 2 : 16;
        e->constants = bsRealloc(e->constants, e->constCap * sizeof(BSValue));
    }
    e->constants[e->constCount] = bsRetain(value);
    return (uint32_t) e->constCount++;
}


static int bsSlotFind(const BSEmit *e, BSValue name)
{
    if (e->slotMap.type != BS_OBJECT) {
        return -1;
    }
    BSValue index = bsObjectGetString(e->slotMap, name);
    return index.type == BS_NUMBER ? (int) index.u.number : -1;
}


static void bsSlotAdd(BSEmit *e, BSValue name)
{
    if (bsSlotFind(e, name) >= 0) {
        return;
    }
    if (e->slotMap.type != BS_OBJECT) {
        e->slotMap = bsObjectNew();
    }
    if (e->slotCount == e->slotCap) {
        e->slotCap = e->slotCap != 0 ? e->slotCap * 2 : 8;
        e->slotNames = bsRealloc(e->slotNames, e->slotCap * sizeof(BSValue));
    }
    BSValue interned = bsInternName(name);
    bsObjectSetString(e->slotMap, interned, bsNumber((double) e->slotCount));
    e->slotNames[e->slotCount++] = interned;
}


/* Allocate a global-name cache site for a CALL_NAME or LOAD_NAME; the operand is its index */
static uint32_t bsEmitSite(BSEmit *e, BSValue name)
{
    if (e->callCount == e->callCap) {
        e->callCap = e->callCap != 0 ? e->callCap * 2 : 8;
        e->callNames = bsRealloc(e->callNames, e->callCap * sizeof(uint32_t));
    }
    e->callNames[e->callCount] = bsEmitConst(e, name);
    return (uint32_t) e->callCount++;
}


/* Emit the slot instruction for a function-local name, or the name instruction with a cache site */
static void bsEmitNamed(BSEmit *e, BSValue interned, uint8_t slotOp, uint8_t nameOp)
{
    int slot = bsSlotFind(e, interned);
    if (slot >= 0) {
        bsEmitInst(e, slotOp, (uint32_t) slot);
    } else {
        bsEmitInst(e, nameOp, bsEmitSite(e, interned));
    }
}


static void bsEmitJump(BSEmit *e, uint8_t op, BSValue label)
{
    /* "jumpif (!expr)" - fold the NOT into the jump, unless another jump lands between them */
    if (op == BS_OP_JUMP_TRUE && e->count != 0 && BS_OP(e->inst[e->count - 1]) == BS_OP_NOT &&
        e->targetAt != e->count) {
        e->count--;
        op = BS_OP_JUMP_FALSE;
    }
    BSValue interned = bsInternName(label);
    BSValue pc = e->labels.type == BS_OBJECT ? bsObjectGetString(e->labels, interned) : bsNull();
    if (pc.type == BS_NUMBER) {
        bsEmitInst(e, op, (uint32_t) pc.u.number);
        bsRelease(interned);
        return;
    }
    if (e->patchCount == e->patchCap) {
        e->patchCap = e->patchCap != 0 ? e->patchCap * 2 : 8;
        e->patches = bsRealloc(e->patches, e->patchCap * sizeof(BSPatch));
    }
    uint32_t at = bsEmitInst(e, op, 0xffffffu);
    e->patches[e->patchCount].pc = at;
    e->patches[e->patchCount].label = interned;
    e->patchCount++;
}


static void bsEmitLabel(BSEmit *e, BSValue name)
{
    if (e->labels.type != BS_OBJECT) {
        e->labels = bsObjectNew();
    }
    BSValue interned = bsInternName(name);
    bsObjectSetString(e->labels, interned, bsNumber((double) e->count));
    bsRelease(interned);
    e->targetAt = e->count;
}


static bool bsEmitExpr(BSEmit *e, BSValue model);


static uint8_t bsBinaryOpcode(const char *op)
{
    static const struct {
        const char *text;
        uint8_t opcode;
    } table[] = {
        {"**", BS_OP_POW}, {"*", BS_OP_MUL}, {"/", BS_OP_DIV}, {"%", BS_OP_MOD},
        {"+", BS_OP_ADD}, {"-", BS_OP_SUB}, {"<<", BS_OP_SHL}, {">>", BS_OP_SHR},
        {"<=", BS_OP_LE}, {"<", BS_OP_LT}, {">=", BS_OP_GE}, {">", BS_OP_GT},
        {"==", BS_OP_EQ}, {"!=", BS_OP_NE}, {"&", BS_OP_BAND}, {"^", BS_OP_BXOR},
        {"|", BS_OP_BOR}
    };
    for (size_t ix = 0; ix < sizeof(table) / sizeof(table[0]); ix++) {
        if (strcmp(table[ix].text, op) == 0) {
            return table[ix].opcode;
        }
    }
    return 0;
}


static bool bsEmitIf(BSEmit *e, BSValue args)
{
    size_t argCount = bsArrayCount(args);
    if (argCount >= 1 && !bsEmitExpr(e, bsArrayGet(args, 0))) {
        return false;
    }
    if (argCount == 0) {
        bsEmitInst(e, BS_OP_LOAD_NULL, 0);
        return true;
    }
    uint32_t jumpElse = bsEmitInst(e, BS_OP_JUMP_FALSE, 0xffffffu);
    if (argCount >= 2) {
        if (!bsEmitExpr(e, bsArrayGet(args, 1))) {
            return false;
        }
    } else {
        bsEmitInst(e, BS_OP_LOAD_NULL, 0);
    }
    uint32_t jumpEnd = bsEmitInst(e, BS_OP_JUMP, 0xffffffu);
    e->inst[jumpElse] = BS_INST(BS_OP_JUMP_FALSE, (uint32_t) e->count);
    e->targetAt = e->count;
    e->depth--; /* the else branch starts without the then branch's value */
    if (argCount >= 3) {
        if (!bsEmitExpr(e, bsArrayGet(args, 2))) {
            return false;
        }
    } else {
        bsEmitInst(e, BS_OP_LOAD_NULL, 0);
    }
    e->inst[jumpEnd] = BS_INST(BS_OP_JUMP, (uint32_t) e->count);
    e->targetAt = e->count;
    return true;
}


static bool bsEmitExpr(BSEmit *e, BSValue model)
{
    if (model.type != BS_OBJECT) {
        return false;
    }

    BSValue number = bsObjectGetString(model, bsKeys.number);
    if (number.type == BS_NUMBER) {
        bsEmitInst(e, BS_OP_LOAD_CONST, bsEmitConst(e, number));
        return true;
    }

    BSValue string = bsObjectGetString(model, bsKeys.string);
    if (string.type == BS_STRING) {
        BSValue interned = bsInternName(string);
        bsEmitInst(e, BS_OP_LOAD_CONST, bsEmitConst(e, interned));
        bsRelease(interned);
        return true;
    }

    BSValue variable = bsObjectGetString(model, bsKeys.variable);
    if (variable.type == BS_STRING) {
        const char *name = bsStringData(variable);
        if (strcmp(name, "null") == 0) {
            bsEmitInst(e, BS_OP_LOAD_NULL, 0);
        } else if (strcmp(name, "true") == 0) {
            bsEmitInst(e, BS_OP_LOAD_TRUE, 0);
        } else if (strcmp(name, "false") == 0) {
            bsEmitInst(e, BS_OP_LOAD_FALSE, 0);
        } else {
            BSValue interned = bsInternName(variable);
            bsEmitNamed(e, interned, BS_OP_LOAD_SLOT, BS_OP_LOAD_NAME);
            bsRelease(interned);
        }
        return true;
    }

    BSValue function = bsObjectGetString(model, bsKeys.function);
    if (function.type == BS_OBJECT) {
        BSValue name = bsObjectGetString(function, bsKeys.name);
        if (name.type != BS_STRING) {
            return false;
        }
        BSValue args = bsObjectGetString(function, bsKeys.args);
        if (strcmp(bsStringData(name), "if") == 0) {
            return bsEmitIf(e, args);
        }
        size_t argCount = bsArrayCount(args);
        for (size_t ix = 0; ix < argCount; ix++) {
            if (!bsEmitExpr(e, bsArrayGet(args, ix))) {
                return false;
            }
        }
        BSValue interned = bsInternName(name);
        bsEmitNamed(e, interned, BS_OP_CALL_SLOT, BS_OP_CALL_NAME);
        /* The following word is the argument count (never dispatched) */
        bsEmitInst(e, BS_OP_ARGC, (uint32_t) argCount);
        bsRelease(interned);
        return true;
    }

    BSValue binary = bsObjectGetString(model, bsKeys.binary);
    if (binary.type == BS_OBJECT) {
        BSValue op = bsObjectGetString(binary, bsKeys.op);
        if (op.type != BS_STRING) {
            return false;
        }
        const char *opText = bsStringData(op);
        bool isAnd = strcmp(opText, "&&") == 0;
        if (isAnd || strcmp(opText, "||") == 0) {
            uint8_t jumpOp = isAnd ? BS_OP_JUMP_FALSE : BS_OP_JUMP_TRUE;
            if (!bsEmitExpr(e, bsObjectGetString(binary, bsKeys.left))) {
                return false;
            }
            bsEmitInst(e, BS_OP_DUP, 0);
            uint32_t jump = bsEmitInst(e, jumpOp, 0xffffffu);
            bsEmitInst(e, BS_OP_POP, 0);
            if (!bsEmitExpr(e, bsObjectGetString(binary, bsKeys.right))) {
                return false;
            }
            e->inst[jump] = BS_INST(jumpOp, (uint32_t) e->count);
            e->targetAt = e->count;
            return true;
        }
        uint8_t opcode = bsBinaryOpcode(opText);
        if (opcode == 0) {
            return false;
        }
        if (!bsEmitExpr(e, bsObjectGetString(binary, bsKeys.left)) || !bsEmitExpr(e, bsObjectGetString(binary, bsKeys.right))) {
            return false;
        }
        bsEmitInst(e, opcode, 0);
        return true;
    }

    BSValue unary = bsObjectGetString(model, bsKeys.unary);
    if (unary.type == BS_OBJECT) {
        BSValue op = bsObjectGetString(unary, bsKeys.op);
        if (op.type != BS_STRING) {
            return false;
        }
        const char *opText = bsStringData(op);
        uint8_t opcode = 0;
        if (strcmp(opText, "-") == 0) {
            opcode = BS_OP_NEG;
        } else if (strcmp(opText, "!") == 0) {
            opcode = BS_OP_NOT;
        } else if (strcmp(opText, "~") == 0) {
            opcode = BS_OP_BNOT;
        } else {
            return false;
        }
        if (!bsEmitExpr(e, bsObjectGetString(unary, bsKeys.expr))) {
            return false;
        }
        bsEmitInst(e, opcode, 0);
        return true;
    }

    if (bsObjectHasString(model, bsKeys.group)) {
        return bsEmitExpr(e, bsObjectGetString(model, bsKeys.group));
    }

    return false;
}


static int bsStatementModelLine(BSValue model)
{
    const BSValue *const keys[] = {
        &bsKeys.expr, &bsKeys.jump, &bsKeys.return_, &bsKeys.label, &bsKeys.function, &bsKeys.include
    };
    for (size_t ix = 0; ix < sizeof(keys) / sizeof(keys[0]); ix++) {
        BSValue inner = bsObjectGetString(model, *keys[ix]);
        if (inner.type == BS_OBJECT) {
            BSValue line = bsObjectGetString(inner, bsKeys.lineNumber);
            return line.type == BS_NUMBER ? (int) line.u.number : 0;
        }
    }
    return 0; /* GCOV_EXCL_LINE - emitCover is only called for a recognized statement */
}


static void bsEmitCover(BSEmit *e, BSValue statementModel)
{
    if (e->coverCount == e->coverCap) {
        e->coverCap = e->coverCap != 0 ? e->coverCap * 2 : 8;
        e->cover = bsRealloc(e->cover, e->coverCap * sizeof(BSValue));
        e->coverLines = bsRealloc(e->coverLines, e->coverCap * sizeof(int));
        e->coverPcs = bsRealloc(e->coverPcs, e->coverCap * sizeof(uint32_t));
    }
    e->cover[e->coverCount] = statementModel;
    e->coverLines[e->coverCount] = bsStatementModelLine(statementModel);
    e->coverPcs[e->coverCount] = (uint32_t) e->count;
    bsEmitInst(e, BS_OP_STMT, (uint32_t) e->coverCount++);
}


static bool bsEmitFunction(BSEmit *e, BSValue model);


static bool bsEmitStatements(BSEmit *e, BSValue statementModels)
{
    size_t count = bsArrayCount(statementModels);
    for (size_t ix = 0; ix < count; ix++) {
        BSValue model = bsArrayGet(statementModels, ix);
        if (model.type != BS_OBJECT) {
            return false;
        }

        BSValue value = bsObjectGetString(model, bsKeys.expr);
        if (value.type == BS_OBJECT) {
            bsEmitCover(e, model);
            if (!bsEmitExpr(e, bsObjectGetString(value, bsKeys.expr))) {
                return false;
            }
            BSValue name = bsObjectGetString(value, bsKeys.name);
            if (name.type == BS_STRING) {
                BSValue interned = bsInternName(name);
                int slot = bsSlotFind(e, interned);
                if (slot >= 0) {
                    bsEmitInst(e, BS_OP_STORE_SLOT, (uint32_t) slot);
                } else {
                    bsEmitInst(e, BS_OP_STORE_NAME, bsEmitConst(e, interned));
                }
                bsRelease(interned);
            } else {
                bsEmitInst(e, BS_OP_POP, 0);
            }
            continue;
        }

        value = bsObjectGetString(model, bsKeys.jump);
        if (value.type == BS_OBJECT) {
            BSValue label = bsObjectGetString(value, bsKeys.label);
            if (label.type != BS_STRING) {
                return false;
            }
            bsEmitCover(e, model);
            if (bsObjectHasString(value, bsKeys.expr)) {
                if (!bsEmitExpr(e, bsObjectGetString(value, bsKeys.expr))) {
                    return false;
                }
                bsEmitJump(e, BS_OP_JUMP_TRUE, label);
            } else {
                bsEmitJump(e, BS_OP_JUMP, label);
            }
            continue;
        }

        value = bsObjectGetString(model, bsKeys.return_);
        if (value.type == BS_OBJECT) {
            bsEmitCover(e, model);
            if (bsObjectHasString(value, bsKeys.expr)) {
                if (!bsEmitExpr(e, bsObjectGetString(value, bsKeys.expr))) {
                    return false;
                }
            } else {
                bsEmitInst(e, BS_OP_LOAD_NULL, 0);
            }
            bsEmitInst(e, BS_OP_RETURN, 0);
            continue;
        }

        value = bsObjectGetString(model, bsKeys.label);
        if (value.type == BS_OBJECT) {
            BSValue name = bsObjectGetString(value, bsKeys.name);
            if (name.type != BS_STRING) {
                return false;
            }
            bsEmitCover(e, model);
            bsEmitLabel(e, name);
            continue;
        }

        value = bsObjectGetString(model, bsKeys.function);
        if (value.type == BS_OBJECT) {
            bsEmitCover(e, model);
            if (!bsEmitFunction(e, value)) {
                return false;
            }
            continue;
        }

        value = bsObjectGetString(model, bsKeys.include);
        if (value.type == BS_OBJECT) {
            BSValue includes = bsObjectGetString(value, bsKeys.includes);
            size_t includeCount = bsArrayCount(includes);
            if (includeCount == 0) {
                return false;
            }
            bsEmitCover(e, model);
            for (size_t inc = 0; inc < includeCount; inc++) {
                BSValue include = bsArrayGet(includes, inc);
                BSValue url = bsObjectGetString(include, bsKeys.url);
                if (include.type != BS_OBJECT || url.type != BS_STRING) {
                    return false;
                }
                if (e->includeCount == e->includeCap) {
                    e->includeCap = e->includeCap != 0 ? e->includeCap * 2 : 4;
                    e->includes = bsRealloc(e->includes, e->includeCap * sizeof(BSInclude));
                }
                e->includes[e->includeCount].url = bsInternName(url);
                e->includes[e->includeCount].system = bsValueBoolean(bsObjectGetString(include, bsKeys.system));
                bsEmitInst(e, BS_OP_INCLUDE, (uint32_t) e->includeCount);
                e->includeCount++;
            }
            continue;
        }

        return false;
    }
    return true;
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
    return low != 0 ? lines[low - 1] : 0;
}


static void bsEmitFinish(BSEmit *e, BSCode *code)
{
    for (size_t ix = 0; ix < e->patchCount; ix++) {
        BSValue pc = e->labels.type == BS_OBJECT ? bsObjectGetString(e->labels, e->patches[ix].label) :
            bsNull();
        uint32_t inst = e->inst[e->patches[ix].pc];
        if (pc.type == BS_NUMBER) {
            e->inst[e->patches[ix].pc] = BS_INST(BS_OP(inst), (uint32_t) pc.u.number);
        } else {
            uint32_t name = bsEmitConst(e, e->patches[ix].label);
            uint8_t op = BS_OP(inst);
            if (op == BS_OP_JUMP) {
                e->inst[e->patches[ix].pc] = BS_INST(BS_OP_JUMP_UNDEF, name);
            } else {
                /*
                 * A jumpif to a missing label only errors if the jump is taken. The trap sits past
                 * the chunk's return, so it carries the jump statement's line in a data word.
                 */
                int line = bsCoverLine(e->coverPcs, e->coverLines, e->coverCount, e->patches[ix].pc);
                uint32_t trap = bsEmitInst(e, BS_OP_JUMP_UNDEF, name);
                bsEmitWord(e, BS_OP_ARGC, (uint32_t) line);
                e->inst[e->patches[ix].pc] = BS_INST(op, trap);
            }
        }
        bsRelease(e->patches[ix].label);
    }
    free(e->patches);
    bsRelease(e->labels);
    bsRelease(e->slotMap);
    bsRelease(e->constMap);

    memset(code, 0, sizeof(*code));
    code->inst = e->inst;
    code->count = e->count;
    code->stackMax = (size_t) e->maxDepth;
    code->constants = e->constants;
    code->constantCount = e->constCount;
    code->includes = e->includes;
    code->includeCount = e->includeCount;
    code->cover = e->cover;
    code->coverLines = e->coverLines;
    code->coverPcs = e->coverPcs;
    code->coverCount = e->coverCount;
    code->slotNames = e->slotNames;
    code->slotCount = e->slotCount;
    if (e->callCount != 0) {
        code->caches = bsAlloc(e->callCount * sizeof(BSCallCache));
        memset(code->caches, 0, e->callCount * sizeof(BSCallCache));
        for (size_t ix = 0; ix < e->callCount; ix++) {
            code->caches[ix].nameIndex = e->callNames[ix];
        }
    }
    free(e->callNames);
}


/* Abandon a failed emit: the buffers built so far form a chunk that bsCodeFree releases */
static void bsEmitDiscard(BSEmit *e)
{
    for (size_t ix = 0; ix < e->patchCount; ix++) {
        bsRelease(e->patches[ix].label);
    }
    free(e->patches);
    free(e->callNames);
    bsRelease(e->labels);
    bsRelease(e->slotMap);
    bsRelease(e->constMap);
    BSCode code = {
        .inst = e->inst, .constants = e->constants, .constantCount = e->constCount,
        .includes = e->includes, .includeCount = e->includeCount, .cover = e->cover,
        .coverLines = e->coverLines, .coverPcs = e->coverPcs, .slotNames = e->slotNames,
        .slotCount = e->slotCount
    };
    bsCodeFree(&code);
}


/* Emit a statement list as a chunk that returns null, into "code". On failure the emit is discarded. */
static bool bsEmitBody(BSEmit *e, BSValue statements, BSCode *code)
{
    if (!bsEmitStatements(e, statements)) {
        bsEmitDiscard(e);
        return false;
    }
    bsEmitInst(e, BS_OP_LOAD_NULL, 0);
    bsEmitInst(e, BS_OP_RETURN, 0);
    bsEmitFinish(e, code);
    return true;
}


static bool bsEmitFunction(BSEmit *e, BSValue model)
{
    BSValue name = bsObjectGetString(model, bsKeys.name);
    BSValue statements = bsObjectGetString(model, bsKeys.statements);
    if (name.type != BS_STRING || statements.type != BS_ARRAY) {
        return false;
    }

    BSFunctionDef *def = bsAlloc(sizeof(BSFunctionDef));
    memset(def, 0, sizeof(*def));
    def->name = bsInternName(name);
    def->lastArgArray = bsValueBoolean(bsObjectGetString(model, bsKeys.lastArgArray));

    BSValue args = bsObjectGetString(model, bsKeys.args);
    size_t argCount = bsArrayCount(args);
    if (argCount != 0) {
        def->argNames = bsAlloc(argCount * sizeof(BSValue));
        for (size_t ix = 0; ix < argCount; ix++) {
            BSValue argName = bsArrayGet(args, ix);
            if (argName.type != BS_STRING) {
                bsFunctionDefFree(def);
                return false;
            }
            def->argNames[def->argCount++] = bsInternName(argName);
        }
    }

    if (e->script->functionCount == *e->functionCap) {
        *e->functionCap = *e->functionCap != 0 ? *e->functionCap * 2 : 8;
        e->script->functions = bsRealloc(e->script->functions, *e->functionCap * sizeof(BSFunctionDef *));
    }
    uint32_t index = (uint32_t) e->script->functionCount;
    e->script->functions[e->script->functionCount++] = def;

    BSEmit body;
    memset(&body, 0, sizeof(body));
    body.script = e->script;
    body.functionCap = e->functionCap;
    for (size_t ix = 0; ix < def->argCount; ix++) {
        bsSlotAdd(&body, def->argNames[ix]);
    }
    size_t stmtCount = bsArrayCount(statements);
    for (size_t ix = 0; ix < stmtCount; ix++) {
        BSValue stmt = bsArrayGet(statements, ix);
        BSValue exprStmt = bsObjectGetString(stmt, bsKeys.expr);
        if (exprStmt.type == BS_OBJECT) {
            BSValue assign = bsObjectGetString(exprStmt, bsKeys.name);
            if (assign.type == BS_STRING) {
                bsSlotAdd(&body, assign);
            }
        }
    }
    if (!bsEmitBody(&body, statements, &def->code)) {
        return false;
    }
    bsEmitInst(e, BS_OP_FUNCTION, index);
    return true;
}


BSExpr *bsExprFromModel(BSValue model)
{
    if (model.type != BS_OBJECT) {
        return NULL;
    }
    bsModelKeysInit();
    BSEmit e;
    memset(&e, 0, sizeof(e));
    if (!bsEmitExpr(&e, model)) {
        bsEmitDiscard(&e);
        return NULL;
    }
    bsEmitInst(&e, BS_OP_RETURN, 0);
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(*expr));
    bsEmitFinish(&e, &expr->code);
    expr->model = bsRetain(model);
    return expr;
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

    BSScript *script = bsAlloc(sizeof(BSScript));
    memset(script, 0, sizeof(*script));
    script->refcount = 1;
    script->system = bsValueBoolean(bsObjectGetString(model, bsKeys.system));
    script->startLineNumber = 1;
    script->model = bsRetain(model);

    BSValue modelName = bsObjectGetString(model, bsKeys.scriptName);
    if (scriptName != NULL) {
        script->scriptName = bsStringNew(scriptName);
    } else {
        script->scriptName = modelName.type == BS_STRING ? bsRetain(modelName) : bsNull();
    }
    BSValue scriptLines = bsObjectGetString(model, bsKeys.scriptLines);
    script->scriptLines = scriptLines.type == BS_ARRAY ? bsRetain(scriptLines) : bsArrayNew();

    size_t functionCap = 0;
    BSEmit e;
    memset(&e, 0, sizeof(e));
    e.script = script;
    e.functionCap = &functionCap;
    if (!bsEmitBody(&e, statements, &script->code)) {
        bsScriptRelease(script);
        return NULL;
    }
    return script;
}


BSValue bsExprToModel(const BSExpr *expr)
{
    return bsRetain(expr->model);
}


BSValue bsScriptToModel(const BSScript *script)
{
    /* A parsed script keeps its lines, not its model - parse them again */
    BSValue source = script->model.type == BS_OBJECT ? bsRetain(script->model) : bsScriptReparse(script);
    BSValue statements = source.type == BS_OBJECT ? bsObjectGet(source, "statements") : bsNull();
    BSValue model = bsObjectNew();
    bsObjectSet(model, "statements",
                statements.type == BS_ARRAY ? bsRetain(statements) : bsArrayNew());
    bsRelease(source);
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
