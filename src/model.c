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
 * model. Data words (call operands, a trap's line) are never STMT and never jumps.
 */
static void bsCodeFinishSystem(BSCode *code)
{
    free(code->cover);
    code->cover = NULL;

    size_t count = code->count;
    uint32_t *map = bsAlloc(count * sizeof(uint32_t));
    BSInst *inst = bsAlloc(count * sizeof(BSInst));
    size_t stripped = 0;
    for (size_t pc = 0; pc < count; pc++) {
        BSInst word = code->inst[pc];
        map[pc] = (uint32_t) stripped;
        if (word.op == BS_OP_STMT) {
            code->coverPcs[word.a] = (uint32_t) stripped;
        } else {
            inst[stripped++] = word;
        }
    }
    for (size_t pc = 0; pc < stripped; pc++) {
        uint8_t op = inst[pc].op;
        if (op == BS_OP_JUMP || op == BS_OP_JUMP_FALSE || op == BS_OP_JUMP_TRUE) {
            inst[pc].w = map[inst[pc].w];
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

    /* The chunks' statement models are borrowed from the model - bsScriptRestoreCover brings them back */
    free(script->code.cover);
    script->code.cover = NULL;
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        free(script->functions[ix]->code.cover);
        script->functions[ix]->code.cover = NULL;
    }
}


/* Move a chunk's statement models from a freshly compiled twin of the same model */
static bool bsCodeTakeCover(BSCode *code, BSCode *twin)
{
    if (twin->coverCount != code->coverCount) {
        return false; /* GCOV_EXCL_LINE - the same model compiles to the same chunks */
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
static _Thread_local struct {
    BSValue args, binary, expr, function, group, include, includes, jump, label, lastArgArray, left, lineNumber, name, number, op, return_, right, scriptLines, scriptName, statements, string, system, unary, url, variable;
} bsKeys;
static _Thread_local bool bsKeysReady;


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


typedef uint16_t BSOperand;

typedef struct {
    BSInst *inst;
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
    BSCallCache *caches;  /* per CALL_NAME, LOAD_NAME, or STORE_NAME site */
    size_t cacheCount;
    size_t cacheCap;
    uint16_t tempTop;     /* the temporaries in use past the slots */
    uint16_t tempMax;
    BSOperand nullConst;  /* the null constant's operand, once allocated */
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
    BSValue labels;
    BSPatch *patches;
    size_t patchCount;
    size_t patchCap;
    BSScript *script;
    size_t *functionCap;
} BSEmit;


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


/* The largest register or constant index an operand can name */
#define BS_OPERAND_MAX 0x7fffu


static uint32_t bsEmitInst(BSEmit *e, uint8_t op, uint16_t a, uint16_t b, uint16_t c)
{
    if (e->count == e->cap) {
        e->cap = e->cap != 0 ? e->cap * 2 : 32;
        e->inst = bsRealloc(e->inst, e->cap * sizeof(BSInst));
    }
    BSInst *inst = &e->inst[e->count];
    inst->op = op;
    inst->x = 0;
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

/* A constant's operand. Interned strings - names and literals - are shared, so a chunk holds each once. */
static BSOperand bsEmitConst(BSEmit *e, BSValue value)
{
    bool interned = value.type == BS_STRING && (value.u.string->flags & BS_STR_INTERNED) != 0;
    if (interned) {
        if (e->constMap.type != BS_OBJECT) {
            e->constMap = bsObjectNew();
        }
        BSValue index = bsObjectGetString(e->constMap, value);
        if (index.type == BS_NUMBER) {
            return (BSOperand) (BS_OPERAND_CONST | (uint32_t) index.u.number);
        }
        bsObjectSetString(e->constMap, value, bsNumber((double) e->constCount));
    }
    if (e->constCount > BS_OPERAND_MAX) {
        e->overflow = true;
    }
    if (e->constCount == e->constCap) {
        e->constCap = e->constCap != 0 ? e->constCap * 2 : 16;
        e->constants = bsRealloc(e->constants, e->constCap * sizeof(BSValue));
    }
    e->constants[e->constCount] = bsRetain(value);
    return (BSOperand) (BS_OPERAND_CONST | (uint32_t) e->constCount++);
}


/* Start a chunk's emit. Constant zero is null, so the null operand never needs allocating. */
static void bsEmitInit(BSEmit *e, BSScript *script, size_t *functionCap)
{
    memset(e, 0, sizeof(*e));
    e->script = script;
    e->functionCap = functionCap;
    e->nullConst = bsEmitConst(e, bsNull());
}


/* Allocate the next temporary register */
static uint16_t bsTempAlloc(BSEmit *e)
{
    size_t index = e->slotCount + e->tempTop;
    if (index > BS_OPERAND_MAX) {
        e->overflow = true;
    }
    e->tempTop++;
    if (e->tempTop > e->tempMax) {
        e->tempMax = e->tempTop;
    }
    return (uint16_t) index;
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


/* Allocate a global-name cache site for a name instruction; the operand is its index */
static uint16_t bsEmitSite(BSEmit *e, BSValue name)
{
    if (e->cacheCount > BS_OPERAND_MAX) {
        e->overflow = true;
    }
    if (e->cacheCount == e->cacheCap) {
        e->cacheCap = e->cacheCap != 0 ? e->cacheCap * 2 : 8;
        e->caches = bsRealloc(e->caches, e->cacheCap * sizeof(BSCallCache));
    }
    BSCallCache *cache = &e->caches[e->cacheCount];
    memset(cache, 0, sizeof(*cache));
    cache->nameIndex = BS_OPERAND_INDEX(bsEmitConst(e, name));
    return (uint16_t) e->cacheCount++;
}


static void bsEmitLabel(BSEmit *e, BSValue name)
{
    if (e->labels.type != BS_OBJECT) {
        e->labels = bsObjectNew();
    }
    BSValue interned = bsInternName(name);
    bsObjectSetString(e->labels, interned, bsNumber((double) e->count));
    bsRelease(interned);
}


/* Emit a jump to a label - patched when the chunk is finished if the label is not yet defined */
static void bsEmitJump(BSEmit *e, uint8_t op, BSOperand cond, BSValue label)
{
    BSValue interned = bsInternName(label);
    BSValue pc = e->labels.type == BS_OBJECT ? bsObjectGetString(e->labels, interned) : bsNull();
    if (pc.type == BS_NUMBER) {
        bsEmitJumpInst(e, op, cond, (uint32_t) pc.u.number);
        bsRelease(interned);
        return;
    }
    if (e->patchCount == e->patchCap) {
        e->patchCap = e->patchCap != 0 ? e->patchCap * 2 : 8;
        e->patches = bsRealloc(e->patches, e->patchCap * sizeof(BSPatch));
    }
    uint32_t at = bsEmitJumpInst(e, op, cond, 0xffffffffu);
    e->patches[e->patchCount].pc = at;
    e->patches[e->patchCount].label = interned;
    e->patchCount++;
}


static bool bsEmitExprTo(BSEmit *e, BSValue model, uint16_t dst);


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
    return e->assignedWords != 0 && (e->assigned[slot / 32] >> (slot % 32)) & 1u;
}


static inline void bsAssignedSet(BSEmit *e, int slot)
{
    if (e->assignedWords != 0) {
        e->assigned[slot / 32] |= (uint32_t) 1 << (slot % 32);
    }
}


/* The slot a statement assigns, or -1 */
static int bsStatementAssigns(const BSEmit *e, BSValue statement)
{
    BSValue expr = bsObjectGetString(statement, bsKeys.expr);
    BSValue name = expr.type == BS_OBJECT ? bsObjectGetString(expr, bsKeys.name) : bsNull();
    if (name.type != BS_STRING) {
        return -1;
    }
    BSValue interned = bsInternName(name);
    int slot = bsSlotFind(e, interned);
    bsRelease(interned);
    return slot;
}


static void bsAssignedAnalyze(BSEmit *e, BSValue statements, size_t argCount)
{
    size_t count = bsArrayCount(statements);
    size_t words = (e->slotCount + 31) / 32;
    if (e->slotCount == 0 || count == 0) {
        return;
    }

    /* Blocks: the first statement, each label, and each statement after a jump or return start one */
    uint8_t *starts = bsAlloc(count);
    memset(starts, 0, count);
    starts[0] = 1;
    BSValue labelBlocks = bsObjectNew(); /* label name -> the array of block indexes that define it */
    for (size_t ix = 0; ix < count; ix++) {
        BSValue statement = bsArrayGet(statements, ix);
        if (bsObjectHasString(statement, bsKeys.label)) {
            starts[ix] = 1;
        } else if ((bsObjectHasString(statement, bsKeys.jump) || bsObjectHasString(statement, bsKeys.return_)) &&
                   ix + 1 < count) {
            starts[ix + 1] = 1;
        }
    }
    size_t blockCount = 0;
    uint32_t *blockOf = bsAlloc(count * sizeof(uint32_t));
    for (size_t ix = 0; ix < count; ix++) {
        if (starts[ix]) {
            blockCount++;
        }
        blockOf[ix] = (uint32_t) (blockCount - 1);
    }
    for (size_t ix = 0; ix < count; ix++) {
        BSValue label = bsObjectGetString(bsArrayGet(statements, ix), bsKeys.label);
        if (label.type == BS_OBJECT) {
            BSValue name = bsObjectGetString(label, bsKeys.name);
            if (name.type == BS_STRING) {
                BSValue blocks = bsObjectGetString(labelBlocks, name);
                if (blocks.type != BS_ARRAY) {
                    blocks = bsArrayNew();
                    bsObjectSetString(labelBlocks, name, blocks);
                }
                bsArrayPush(blocks, bsNumber((double) blockOf[ix]));
            }
        }
    }

    /* Each block's assignments, and its successors: the next block unless it ends in a jump or return */
    uint32_t *gen = bsAlloc(blockCount * words * sizeof(uint32_t));
    memset(gen, 0, blockCount * words * sizeof(uint32_t));
    uint32_t *in = bsAlloc(blockCount * words * sizeof(uint32_t));
    memset(in, 0xff, blockCount * words * sizeof(uint32_t));
    memset(in, 0, words * sizeof(uint32_t));
    for (size_t ix = 0; ix < argCount && ix < e->slotCount; ix++) {
        in[ix / 32] |= (uint32_t) 1 << (ix % 32);
    }
    for (size_t ix = 0; ix < count; ix++) {
        int slot = bsStatementAssigns(e, bsArrayGet(statements, ix));
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
            BSValue statement = bsArrayGet(statements, ix);
            BSValue jump = bsObjectGetString(statement, bsKeys.jump);
            bool fallsThrough = jump.type != BS_OBJECT ? !bsObjectHasString(statement, bsKeys.return_) :
                bsObjectHasString(jump, bsKeys.expr);
            BSValue targets = jump.type == BS_OBJECT ?
                bsObjectGetString(labelBlocks, bsObjectGetString(jump, bsKeys.label)) : bsNull();
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
    free(starts);
    bsRelease(labelBlocks);
}


static void bsAssignedFree(BSEmit *e)
{
    free(e->assigned);
    free(e->blockOf);
    free(e->blockIn);
    e->assigned = NULL;
    e->blockOf = NULL;
    e->blockIn = NULL;
    e->assignedWords = 0;
}


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


/* Whether an expression model computes its value - a call or an operator - rather than naming one */
static bool bsExprComputes(BSValue model)
{
    return bsObjectHasString(model, bsKeys.function) || bsObjectHasString(model, bsKeys.binary) ||
        bsObjectHasString(model, bsKeys.unary);
}


/*
 * Compile an expression to an operand: a constant or a local costs no instruction; anything else
 * is computed into the lowest free temporary, which stays allocated for the caller to consume.
 */
static bool bsEmitExprOperand(BSEmit *e, BSValue model, BSOperand *operand)
{
    if (model.type != BS_OBJECT) {
        return false;
    }

    BSValue number = bsObjectGetString(model, bsKeys.number);
    if (number.type == BS_NUMBER) {
        *operand = bsEmitConst(e, number);
        return true;
    }

    BSValue string = bsObjectGetString(model, bsKeys.string);
    if (string.type == BS_STRING) {
        BSValue interned = bsInternName(string);
        *operand = bsEmitConst(e, interned);
        bsRelease(interned);
        return true;
    }

    BSValue variable = bsObjectGetString(model, bsKeys.variable);
    if (variable.type == BS_STRING) {
        const char *name = bsStringData(variable);
        if (strcmp(name, "null") == 0) {
            *operand = e->nullConst;
            return true;
        }
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
            *operand = bsEmitConst(e, bsBoolean(name[0] == 't'));
            return true;
        }
        BSValue interned = bsInternName(variable);
        int slot = bsSlotFind(e, interned);
        bsRelease(interned);
        if (slot >= 0 && bsAssignedTest(e, slot)) {
            *operand = (BSOperand) slot;
            return true;
        }
        if (slot >= 0) {
            *operand = bsTempAlloc(e);
            bsEmitInst(e, BS_OP_LOAD_SLOT, *operand, (uint16_t) slot, 0);
            return true;
        }
    }

    if (bsObjectHasString(model, bsKeys.group)) {
        return bsEmitExprOperand(e, bsObjectGetString(model, bsKeys.group), operand);
    }

    /* A global variable loads into a temporary; a call or an operator computes into one */
    if (variable.type != BS_STRING && !bsExprComputes(model)) {
        return false;
    }
    *operand = bsTempAlloc(e);
    return bsEmitExprTo(e, model, *operand);
}


/* The conditional: if(cond, then, else) - the value of the branch taken, or null */
static bool bsEmitIfTo(BSEmit *e, BSValue args, uint16_t dst)
{
    size_t argCount = bsArrayCount(args);
    BSOperand null = e->nullConst;
    if (argCount == 0) {
        bsEmitInst(e, BS_OP_MOVE, dst, null, 0);
        return true;
    }
    uint16_t base = e->tempTop;
    BSOperand cond;
    if (!bsEmitExprOperand(e, bsArrayGet(args, 0), &cond)) {
        return false;
    }
    e->tempTop = base;
    uint32_t jumpElse = bsEmitJumpInst(e, BS_OP_JUMP_FALSE, cond, 0);
    if (argCount >= 2) {
        if (!bsEmitExprTo(e, bsArrayGet(args, 1), dst)) {
            return false;
        }
    } else {
        bsEmitInst(e, BS_OP_MOVE, dst, null, 0);
    }
    uint32_t jumpEnd = bsEmitJumpInst(e, BS_OP_JUMP, 0, 0);
    e->inst[jumpElse].w = (uint32_t) e->count;
    if (argCount >= 3) {
        if (!bsEmitExprTo(e, bsArrayGet(args, 2), dst)) {
            return false;
        }
    } else {
        bsEmitInst(e, BS_OP_MOVE, dst, null, 0);
    }
    e->inst[jumpEnd].w = (uint32_t) e->count;
    return true;
}


/* A call: the arguments are operands in the DATA words that follow the call instruction */
static bool bsEmitCallTo(BSEmit *e, BSValue function, uint16_t dst)
{
    BSValue name = bsObjectGetString(function, bsKeys.name);
    BSValue args = bsObjectGetString(function, bsKeys.args);
    size_t argCount = bsArrayCount(args);
    if (argCount > BS_OPERAND_MAX) {
        e->overflow = true;
    }
    uint16_t base = e->tempTop;
    BSOperand argInline[16];
    BSOperand *operands = argCount <= 16 ? argInline : bsAlloc(argCount * sizeof(BSOperand));
    bool ok = true;
    for (size_t ix = 0; ok && ix < argCount; ix++) {
        ok = bsEmitExprOperand(e, bsArrayGet(args, ix), &operands[ix]);
    }
    if (ok) {
        e->tempTop = base;
        BSValue interned = bsInternName(name);
        int slot = bsSlotFind(e, interned);
        if (slot >= 0) {
            bsEmitInst(e, BS_OP_CALL_SLOT, dst, (uint16_t) slot, (uint16_t) argCount);
        } else {
            bsEmitInst(e, BS_OP_CALL_NAME, dst, bsEmitSite(e, interned), (uint16_t) argCount);
        }
        bsRelease(interned);
        for (size_t ix = 0; ix < argCount; ix += BS_OPERANDS_PER_DATA) {
            bsEmitInst(e, BS_OP_DATA, operands[ix],
                       ix + 1 < argCount ? operands[ix + 1] : 0,
                       ix + 2 < argCount ? operands[ix + 2] : 0);
        }
    }
    if (operands != argInline) {
        free(operands);
    }
    return ok;
}


/* Compile an expression so its value lands in register "dst" */
static bool bsEmitExprTo(BSEmit *e, BSValue model, uint16_t dst)
{
    if (model.type != BS_OBJECT) {
        return false;
    }

    BSValue function = bsObjectGetString(model, bsKeys.function);
    if (function.type == BS_OBJECT) {
        BSValue name = bsObjectGetString(function, bsKeys.name);
        if (name.type != BS_STRING) {
            return false;
        }
        if (strcmp(bsStringData(name), "if") != 0) {
            return bsEmitCallTo(e, function, dst);
        }
        /*
         * A conditional or a short-circuit operator writes dst before its later operands are
         * evaluated, so when dst is a named local those operands might read the new value - they
         * accumulate in a temporary instead
         */
        if (dst < e->slotCount) {
            uint16_t base = e->tempTop;
            uint16_t temp = bsTempAlloc(e);
            if (!bsEmitIfTo(e, bsObjectGetString(function, bsKeys.args), temp)) {
                return false;
            }
            bsEmitInst(e, BS_OP_MOVE, dst, temp, 0);
            e->tempTop = base;
            return true;
        }
        return bsEmitIfTo(e, bsObjectGetString(function, bsKeys.args), dst);
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
            uint16_t base = e->tempTop;
            uint16_t acc = dst < e->slotCount ? bsTempAlloc(e) : dst;
            if (!bsEmitExprTo(e, bsObjectGetString(binary, bsKeys.left), acc)) {
                return false;
            }
            uint32_t jump = bsEmitJumpInst(e, isAnd ? BS_OP_JUMP_FALSE : BS_OP_JUMP_TRUE, acc, 0);
            if (!bsEmitExprTo(e, bsObjectGetString(binary, bsKeys.right), acc)) {
                return false;
            }
            e->inst[jump].w = (uint32_t) e->count;
            if (acc != dst) {
                bsEmitInst(e, BS_OP_MOVE, dst, acc, 0);
                e->tempTop = base;
            }
            return true;
        }
        uint8_t opcode = bsBinaryOpcode(opText);
        if (opcode == 0) {
            return false;
        }
        uint16_t base = e->tempTop;
        BSOperand left;
        BSOperand right;
        if (!bsEmitExprOperand(e, bsObjectGetString(binary, bsKeys.left), &left) ||
            !bsEmitExprOperand(e, bsObjectGetString(binary, bsKeys.right), &right)) {
            return false;
        }
        e->tempTop = base;
        bsEmitInst(e, opcode, dst, left, right);
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
        uint16_t base = e->tempTop;
        BSOperand operand;
        if (!bsEmitExprOperand(e, bsObjectGetString(unary, bsKeys.expr), &operand)) {
            return false;
        }
        e->tempTop = base;
        bsEmitInst(e, opcode, dst, operand, 0);
        return true;
    }

    if (bsObjectHasString(model, bsKeys.group)) {
        return bsEmitExprTo(e, bsObjectGetString(model, bsKeys.group), dst);
    }

    /* A global variable or an unassigned local loads into dst; a constant or an assigned local moves */
    BSValue variable = bsObjectGetString(model, bsKeys.variable);
    if (variable.type == BS_STRING) {
        const char *name = bsStringData(variable);
        if (strcmp(name, "null") != 0 && strcmp(name, "true") != 0 && strcmp(name, "false") != 0) {
            BSValue interned = bsInternName(variable);
            int slot = bsSlotFind(e, interned);
            if (slot < 0) {
                bsEmitInst(e, BS_OP_LOAD_NAME, dst, bsEmitSite(e, interned), 0);
                bsRelease(interned);
                return true;
            }
            bsRelease(interned);
            if (!bsAssignedTest(e, slot)) {
                bsEmitInst(e, BS_OP_LOAD_SLOT, dst, (uint16_t) slot, 0);
                return true;
            }
        }
    }
    if (bsExprComputes(model)) {
        return false; /* a malformed call or operator */
    }
    BSOperand operand;
    if (!bsEmitExprOperand(e, model, &operand)) {
        return false;
    }
    if (operand != dst) {
        bsEmitInst(e, BS_OP_MOVE, dst, operand, 0);
    }
    return true;
}


/* Compile an expression statement - a call drops its result; anything else is computed and left */
static bool bsEmitExprDiscard(BSEmit *e, BSValue model)
{
    BSValue function = bsObjectGetString(model, bsKeys.function);
    BSValue name = bsObjectGetString(function, bsKeys.name);
    if (name.type == BS_STRING && strcmp(bsStringData(name), "if") != 0) {
        return bsEmitCallTo(e, function, BS_REG_DISCARD);
    }
    uint16_t base = e->tempTop;
    BSOperand operand;
    if (!bsEmitExprOperand(e, model, &operand)) {
        return false;
    }
    e->tempTop = base;
    return true;
}


/* Record a statement's model and line - "kind" is the statement's member object, which carries the line */
static void bsEmitCover(BSEmit *e, BSValue statementModel, BSValue kind)
{
    if (e->coverCount == e->coverCap) {
        e->coverCap = e->coverCap != 0 ? e->coverCap * 2 : 8;
        e->cover = bsRealloc(e->cover, e->coverCap * sizeof(BSValue));
        e->coverLines = bsRealloc(e->coverLines, e->coverCap * sizeof(int));
        e->coverPcs = bsRealloc(e->coverPcs, e->coverCap * sizeof(uint32_t));
    }
    e->cover[e->coverCount] = statementModel;
    BSValue line = bsObjectGetString(kind, bsKeys.lineNumber);
    e->coverLines[e->coverCount] = line.type == BS_NUMBER ? (int) line.u.number : 0;
    e->coverPcs[e->coverCount] = (uint32_t) e->count;
    bsEmitInst(e, BS_OP_STMT, (uint16_t) e->coverCount, 0, 0);
    e->coverCount++;
}


static bool bsEmitFunction(BSEmit *e, BSValue model);


/* Emit one statement model. Returns false for a malformed statement. */
static bool bsEmitStatement(BSEmit *e, BSValue model)
{
    if (model.type != BS_OBJECT) {
        return false;
    }

    BSValue value = bsObjectGetString(model, bsKeys.expr);
    if (value.type == BS_OBJECT) {
        bsEmitCover(e, model, value);
        BSValue expr = bsObjectGetString(value, bsKeys.expr);
        BSValue name = bsObjectGetString(value, bsKeys.name);
        if (name.type != BS_STRING) {
            return bsEmitExprDiscard(e, expr);
        }
        BSValue interned = bsInternName(name);
        int slot = bsSlotFind(e, interned);
        if (slot >= 0) {
            bsRelease(interned);
            if (!bsEmitExprTo(e, expr, (uint16_t) slot)) {
                return false;
            }
            bsAssignedSet(e, slot);
            return true;
        }
        uint16_t site = bsEmitSite(e, interned);
        bsRelease(interned);
        uint16_t base = e->tempTop;
        BSOperand operand;
        if (!bsEmitExprOperand(e, expr, &operand)) {
            return false;
        }
        e->tempTop = base;
        bsEmitInst(e, BS_OP_STORE_NAME, site, operand, 0);
        return true;
    }

    value = bsObjectGetString(model, bsKeys.jump);
    if (value.type == BS_OBJECT) {
        BSValue label = bsObjectGetString(value, bsKeys.label);
        if (label.type != BS_STRING) {
            return false;
        }
        bsEmitCover(e, model, value);
        if (!bsObjectHasString(value, bsKeys.expr)) {
            bsEmitJump(e, BS_OP_JUMP, 0, label);
            return true;
        }
        /* "jumpif (!expr)" - the NOT folds into the jump */
        BSValue expr = bsObjectGetString(value, bsKeys.expr);
        uint8_t op = BS_OP_JUMP_TRUE;
        BSValue unary = expr.type == BS_OBJECT ? bsObjectGetString(expr, bsKeys.unary) : bsNull();
        if (unary.type == BS_OBJECT) {
            BSValue unaryOp = bsObjectGetString(unary, bsKeys.op);
            if (unaryOp.type == BS_STRING && strcmp(bsStringData(unaryOp), "!") == 0) {
                expr = bsObjectGetString(unary, bsKeys.expr);
                op = BS_OP_JUMP_FALSE;
            }
        }
        uint16_t base = e->tempTop;
        BSOperand cond;
        if (!bsEmitExprOperand(e, expr, &cond)) {
            return false;
        }
        e->tempTop = base;
        bsEmitJump(e, op, cond, label);
        return true;
    }

    value = bsObjectGetString(model, bsKeys.return_);
    if (value.type == BS_OBJECT) {
        bsEmitCover(e, model, value);
        uint16_t base = e->tempTop;
        BSOperand operand;
        if (bsObjectHasString(value, bsKeys.expr)) {
            if (!bsEmitExprOperand(e, bsObjectGetString(value, bsKeys.expr), &operand)) {
                return false;
            }
        } else {
            operand = e->nullConst;
        }
        e->tempTop = base;
        bsEmitInst(e, BS_OP_RETURN, operand, 0, 0);
        return true;
    }

    value = bsObjectGetString(model, bsKeys.label);
    if (value.type == BS_OBJECT) {
        BSValue name = bsObjectGetString(value, bsKeys.name);
        if (name.type != BS_STRING) {
            return false;
        }
        bsEmitCover(e, model, value);
        bsEmitLabel(e, name);
        return true;
    }

    value = bsObjectGetString(model, bsKeys.function);
    if (value.type == BS_OBJECT) {
        bsEmitCover(e, model, value);
        return bsEmitFunction(e, value);
    }

    value = bsObjectGetString(model, bsKeys.include);
    if (value.type == BS_OBJECT) {
        BSValue includes = bsObjectGetString(value, bsKeys.includes);
        size_t includeCount = bsArrayCount(includes);
        if (includeCount == 0) {
            return false;
        }
        bsEmitCover(e, model, value);
        for (size_t inc = 0; inc < includeCount; inc++) {
            BSValue include = bsArrayGet(includes, inc);
            BSValue url = bsObjectGetString(include, bsKeys.url);
            if (include.type != BS_OBJECT || url.type != BS_STRING) {
                return false;
            }
            if (e->includeCount > BS_OPERAND_MAX) {
                e->overflow = true;
            }
            if (e->includeCount == e->includeCap) {
                e->includeCap = e->includeCap != 0 ? e->includeCap * 2 : 4;
                e->includes = bsRealloc(e->includes, e->includeCap * sizeof(BSInclude));
            }
            e->includes[e->includeCount].url = bsInternName(url);
            e->includes[e->includeCount].system = bsValueBoolean(bsObjectGetString(include, bsKeys.system));
            bsEmitInst(e, BS_OP_INCLUDE, (uint16_t) e->includeCount, 0, 0);
            e->includeCount++;
        }
        return true;
    }

    return false;
}


static bool bsEmitStatements(BSEmit *e, BSValue statementModels)
{
    size_t count = bsArrayCount(statementModels);
    for (size_t ix = 0; ix < count; ix++) {
        if (e->assignedWords != 0 && (ix == 0 || e->blockOf[ix] != e->blockOf[ix - 1])) {
            memcpy(e->assigned, &e->blockIn[e->blockOf[ix] * e->assignedWords],
                   e->assignedWords * sizeof(uint32_t));
        }
        if (!bsEmitStatement(e, bsArrayGet(statementModels, ix))) {
            return false;
        }
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
    return low == 0 ? 0 : lines[low - 1];
}


/*
 * Finish a chunk into "code", resolving its forward jumps. Returns false if an operand space
 * overflowed - the chunk is then invalid, but complete, so bsCodeFree releases it.
 */
static bool bsEmitFinish(BSEmit *e, BSCode *code)
{
    for (size_t ix = 0; ix < e->patchCount; ix++) {
        BSValue pc = e->labels.type == BS_OBJECT ? bsObjectGetString(e->labels, e->patches[ix].label) :
            bsNull();
        BSInst *inst = &e->inst[e->patches[ix].pc];
        if (pc.type == BS_NUMBER) {
            inst->w = (uint32_t) pc.u.number;
        } else {
            BSOperand name = bsEmitConst(e, e->patches[ix].label);
            if (inst->op == BS_OP_JUMP) {
                inst->op = BS_OP_JUMP_UNDEF;
                inst->a = BS_OPERAND_INDEX(name);
            } else {
                /*
                 * A jumpif to a missing label only errors if the jump is taken. The trap sits past
                 * the chunk's return, so it carries the jump statement's line in a data word.
                 */
                int line = bsCoverLine(e->coverPcs, e->coverLines, e->coverCount, e->patches[ix].pc);
                uint32_t trap = bsEmitInst(e, BS_OP_JUMP_UNDEF, BS_OPERAND_INDEX(name), 0, 0);
                bsEmitJumpInst(e, BS_OP_DATA, 0, (uint32_t) line);
                e->inst[e->patches[ix].pc].w = trap;
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
    code->tempCount = e->tempMax;
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
    code->caches = e->caches;
    return !e->overflow;
}


/*
 * Emit a statement list as a chunk that returns null, into "code". Returns false - the chunk
 * released - for a malformed statement or an overflowed operand space.
 */
static bool bsEmitBody(BSEmit *e, BSValue statements, BSCode *code)
{
    bool emitted = bsEmitStatements(e, statements);
    bsEmitInst(e, BS_OP_RETURN, e->nullConst, 0, 0);
    bool finished = bsEmitFinish(e, code);
    if (!emitted || !finished) {
        bsCodeFree(code);
        return false;
    }
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
    bsEmitInit(&body, e->script, e->functionCap);
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
    bsAssignedAnalyze(&body, statements, def->argCount);
    bool emitted = bsEmitBody(&body, statements, &def->code);
    bsAssignedFree(&body);
    if (!emitted) {
        return false;
    }
    if (index > 0xffffu) {
        e->overflow = true;
    }
    bsEmitInst(e, BS_OP_FUNCTION, (uint16_t) index, 0, 0);
    return true;
}


BSExpr *bsExprFromModel(BSValue model)
{
    bsModelKeysInit();
    BSEmit e;
    bsEmitInit(&e, NULL, NULL);
    BSOperand operand = e.nullConst;
    bool emitted = bsEmitExprOperand(&e, model, &operand);
    bsEmitInst(&e, BS_OP_RETURN, operand, 0, 0);
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(*expr));
    bool finished = bsEmitFinish(&e, &expr->code);
    if (!emitted || !finished) {
        bsExprFree(expr);
        return NULL;
    }
    expr->model = bsRetain(model);
    return expr;
}


/* A new script with no name, no lines, and no model */
static BSScript *bsScriptNew(void)
{
    BSScript *script = bsAlloc(sizeof(BSScript));
    memset(script, 0, sizeof(*script));
    script->refcount = 1;
    script->startLineNumber = 1;
    script->model = bsNull();
    script->scriptName = bsNull();
    script->scriptLines = bsArrayNew();
    return script;
}


/* Take a script's name, lines, and system flag from its model's members; "scriptName" overrides the name */
static void bsScriptInfo(BSScript *script, BSValue model, const char *scriptName)
{
    script->system = bsValueBoolean(bsObjectGetString(model, bsKeys.system));
    BSValue modelName = bsObjectGetString(model, bsKeys.scriptName);
    if (scriptName != NULL) {
        bsAssign(&script->scriptName, bsStringNew(scriptName));
    } else if (modelName.type == BS_STRING) {
        bsAssign(&script->scriptName, bsRetain(modelName));
    }
    BSValue scriptLines = bsObjectGetString(model, bsKeys.scriptLines);
    if (scriptLines.type == BS_ARRAY) {
        bsAssign(&script->scriptLines, bsRetain(scriptLines));
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

    BSScript *script = bsScriptNew();
    script->model = bsRetain(model);
    bsScriptInfo(script, model, scriptName);

    size_t functionCap = 0;
    BSEmit e;
    bsEmitInit(&e, script, &functionCap);
    if (!bsEmitBody(&e, statements, &script->code)) {
        bsScriptRelease(script);
        return NULL;
    }
    return script;
}


/* Emit a streamed statement; an overflow is reported as the decoder's invalid-model error */
static bool bsEmitStreamedStatement(BSValue statement, void *data)
{
    BSEmit *e = data;
    return bsEmitStatement(e, statement) && !e->overflow;
}


BSScript *bsScriptFromModelJSON(const char *text, size_t size, const char *scriptName, const char **error)
{
    bsModelKeysInit();
    BSScript *script = bsScriptNew();
    size_t functionCap = 0;
    BSEmit e;
    bsEmitInit(&e, script, &functionCap);
    BSValue rest;
    bool decoded = bsJSONDecodeStatements(text, size, bsEmitStreamedStatement, &e, &rest, error);
    bsEmitInst(&e, BS_OP_RETURN, e.nullConst, 0, 0);
    bool finished = bsEmitFinish(&e, &script->code);
    if (!decoded || !finished) {
        bsRelease(rest);
        if (error != NULL && *error == NULL) {
            *error = "Invalid BareScript model";
        }
        bsScriptRelease(script);
        return NULL;
    }
    bsScriptInfo(script, rest, scriptName);
    bsRelease(rest);

    /* The statement models the chunks borrowed were released as they were compiled */
    bsScriptForgetModel(script);
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
