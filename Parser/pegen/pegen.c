#include <Python.h>
#include <errcode.h>
#include "../tokenizer.h"

#include "pegen.h"
#include "parse_string.h"

static int
init_normalization(Parser *p)
{
    PyObject *m = PyImport_ImportModuleNoBlock("unicodedata");
    if (!m)
    {
        return 0;
    }
    p->normalize = PyObject_GetAttrString(m, "normalize");
    Py_DECREF(m);
    if (!p->normalize)
    {
        return 0;
    }
    return 1;
}

PyObject *
_PyPegen_new_identifier(Parser *p, char *n)
{
    PyObject *id = PyUnicode_DecodeUTF8(n, strlen(n), NULL);
    if (!id) {
        goto error;
    }
    /* PyUnicode_DecodeUTF8 should always return a ready string. */
    assert(PyUnicode_IS_READY(id));
    /* Check whether there are non-ASCII characters in the
       identifier; if so, normalize to NFKC. */
    if (!PyUnicode_IS_ASCII(id))
    {
        PyObject *id2;
        if (!p->normalize && !init_normalization(p))
        {
            Py_DECREF(id);
            goto error;
        }
        PyObject *form = PyUnicode_InternFromString("NFKC");
        if (form == NULL)
        {
            Py_DECREF(id);
            goto error;
        }
        PyObject *args[2] = {form, id};
        id2 = _PyObject_FastCall(p->normalize, args, 2);
        Py_DECREF(id);
        Py_DECREF(form);
        if (!id2) {
            goto error;
        }
        if (!PyUnicode_Check(id2))
        {
            PyErr_Format(PyExc_TypeError,
                         "unicodedata.normalize() must return a string, not "
                         "%.200s",
                         _PyType_Name(Py_TYPE(id2)));
            Py_DECREF(id2);
            goto error;
        }
        id = id2;
    }
    PyUnicode_InternInPlace(&id);
    if (PyArena_AddPyObject(p->arena, id) < 0)
    {
        Py_DECREF(id);
        goto error;
    }
    return id;

error:
    p->error_indicator = 1;
    return NULL;
}

static PyObject *
_create_dummy_identifier(Parser *p)
{
    return _PyPegen_new_identifier(p, "");
}

static inline Py_ssize_t
byte_offset_to_character_offset(PyObject *line, int col_offset)
{
    const char *str = PyUnicode_AsUTF8(line);
    PyObject *text = PyUnicode_DecodeUTF8(str, col_offset, NULL);
    if (!text) {
        return 0;
    }
    Py_ssize_t size = PyUnicode_GET_LENGTH(text);
    Py_DECREF(text);
    return size;
}

const char *
_PyPegen_get_expr_name(expr_ty e)
{
    switch (e->kind) {
        case Attribute_kind:
            return "attribute";
        case Subscript_kind:
            return "subscript";
        case Starred_kind:
            return "starred";
        case Name_kind:
            return "name";
        case List_kind:
            return "list";
        case Tuple_kind:
            return "tuple";
        case Lambda_kind:
            return "lambda";
        case Call_kind:
            return "function call";
        case BoolOp_kind:
        case BinOp_kind:
        case UnaryOp_kind:
            return "operator";
        case GeneratorExp_kind:
            return "generator expression";
        case Yield_kind:
        case YieldFrom_kind:
            return "yield expression";
        case Await_kind:
            return "await expression";
        case ListComp_kind:
            return "list comprehension";
        case SetComp_kind:
            return "set comprehension";
        case DictComp_kind:
            return "dict comprehension";
        case Dict_kind:
            return "dict display";
        case Set_kind:
            return "set display";
        case JoinedStr_kind:
        case FormattedValue_kind:
            return "f-string expression";
        case Constant_kind: {
            PyObject *value = e->v.Constant.value;
            if (value == Py_None) {
                return "None";
            }
            if (value == Py_False) {
                return "False";
            }
            if (value == Py_True) {
                return "True";
            }
            if (value == Py_Ellipsis) {
                return "Ellipsis";
            }
            return "literal";
        }
        case Compare_kind:
            return "comparison";
        case IfExp_kind:
            return "conditional expression";
        case NamedExpr_kind:
            return "named expression";
        default:
            PyErr_Format(PyExc_SystemError,
                         "unexpected expression in assignment %d (line %d)",
                         e->kind, e->lineno);
            return NULL;
    }
}

static void
raise_decode_error(Parser *p)
{
    const char *errtype = NULL;
    if (PyErr_ExceptionMatches(PyExc_UnicodeError)) {
        errtype = "unicode error";
    }
    else if (PyErr_ExceptionMatches(PyExc_ValueError)) {
        errtype = "value error";
    }
    if (errtype) {
        PyObject *type, *value, *tback, *errstr;
        PyErr_Fetch(&type, &value, &tback);
        errstr = PyObject_Str(value);
        if (errstr) {
            RAISE_SYNTAX_ERROR("(%s) %U", errtype, errstr);
            Py_DECREF(errstr);
        }
        else {
            PyErr_Clear();
            RAISE_SYNTAX_ERROR("(%s) unknown error", errtype);
        }
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tback);
    }
}

static void
raise_tokenizer_init_error(PyObject *filename)
{
    if (!(PyErr_ExceptionMatches(PyExc_LookupError)
          || PyErr_ExceptionMatches(PyExc_ValueError)
          || PyErr_ExceptionMatches(PyExc_UnicodeDecodeError))) {
        return;
    }
    PyObject *type, *value, *tback, *errstr;
    PyErr_Fetch(&type, &value, &tback);
    errstr = PyObject_Str(value);

    Py_INCREF(Py_None);
    PyObject *tmp = Py_BuildValue("(OiiN)", filename, 0, -1, Py_None);
    if (!tmp) {
        goto error;
    }

    value = PyTuple_Pack(2, errstr, tmp);
    Py_DECREF(tmp);
    if (!value) {
        goto error;
    }
    PyErr_SetObject(PyExc_SyntaxError, value);

error:
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tback);
}

static inline PyObject *
get_error_line(char *buffer)
{
    char *newline = strchr(buffer, '\n');
    if (newline) {
        return PyUnicode_FromStringAndSize(buffer, newline - buffer);
    }
    else {
        return PyUnicode_FromString(buffer);
    }
}

static int
tokenizer_error_with_col_offset(Parser *p, PyObject *errtype, const char *errmsg)
{
    PyObject *errstr = NULL;
    PyObject *value = NULL;
    int col_number = -1;

    errstr = PyUnicode_FromString(errmsg);
    if (!errstr) {
        return -1;
    }

    PyObject *loc = NULL;
    if (p->start_rule == Py_file_input) {
        loc = PyErr_ProgramTextObject(p->tok->filename, p->tok->lineno);
    }
    if (!loc) {
        loc = get_error_line(p->tok->buf);
    }

    if (loc) {
        col_number = p->tok->cur - p->tok->buf;
    }
    else {
        Py_INCREF(Py_None);
        loc = Py_None;
    }

    PyObject *tmp = Py_BuildValue("(OiiN)", p->tok->filename, p->tok->lineno,
                                  col_number, loc);
    if (!tmp) {
        goto error;
    }

    value = PyTuple_Pack(2, errstr, tmp);
    Py_DECREF(tmp);
    if (!value) {
        goto error;
    }
    PyErr_SetObject(errtype, value);

    Py_XDECREF(value);
    Py_XDECREF(errstr);
    return -1;

error:
    Py_XDECREF(errstr);
    Py_XDECREF(loc);
    return -1;
}

static int
tokenizer_error(Parser *p)
{
    if (PyErr_Occurred()) {
        return -1;
    }

    const char *msg = NULL;
    PyObject* errtype = PyExc_SyntaxError;
    switch (p->tok->done) {
        case E_TOKEN:
            msg = "invalid token";
            break;
        case E_IDENTIFIER:
            msg = "invalid character in identifier";
            break;
        case E_BADPREFIX:
            return tokenizer_error_with_col_offset(p,
                PyExc_SyntaxError, "invalid string prefix");
        case E_EOFS:
            return tokenizer_error_with_col_offset(p,
                PyExc_SyntaxError, "EOF while scanning triple-quoted string literal");
        case E_EOLS:
            return tokenizer_error_with_col_offset(p,
                PyExc_SyntaxError, "EOL while scanning string literal");
        case E_DEDENT:
            return tokenizer_error_with_col_offset(p,
                PyExc_IndentationError, "unindent does not match any outer indentation level");
        case E_INTR:
            if (!PyErr_Occurred()) {
                PyErr_SetNone(PyExc_KeyboardInterrupt);
            }
            return -1;
        case E_NOMEM:
            PyErr_NoMemory();
            return -1;
        case E_TABSPACE:
            errtype = PyExc_TabError;
            msg = "inconsistent use of tabs and spaces in indentation";
            break;
        case E_TOODEEP:
            errtype = PyExc_IndentationError;
            msg = "too many levels of indentation";
            break;
        case E_DECODE:
            raise_decode_error(p);
            return -1;
        case E_LINECONT:
            msg = "unexpected character after line continuation character";
            break;
        default:
            msg = "unknown parsing error";
    }

    PyErr_Format(errtype, msg);
    // There is no reliable column information for this error
    PyErr_SyntaxLocationObject(p->tok->filename, p->tok->lineno, 0);

    return -1;
}

void *
_PyPegen_raise_error(Parser *p, PyObject *errtype, const char *errmsg, ...)
{
    PyObject *value = NULL;
    PyObject *errstr = NULL;
    PyObject *loc = NULL;
    PyObject *tmp = NULL;
    Token *t = p->tokens[p->fill - 1];
    Py_ssize_t col_number = 0;
    va_list va;

    va_start(va, errmsg);
    errstr = PyUnicode_FromFormatV(errmsg, va);
    va_end(va);
    if (!errstr) {
        goto error;
    }

    if (p->start_rule == Py_file_input) {
        loc = PyErr_ProgramTextObject(p->tok->filename, t->lineno);
    }

    if (!loc) {
        loc = get_error_line(p->tok->buf);
    }

    if (loc) {
        int col_offset = t->col_offset == -1 ? 0 : t->col_offset;
        col_number = byte_offset_to_character_offset(loc, col_offset) + 1;
    }
    else {
        Py_INCREF(Py_None);
        loc = Py_None;
    }


    tmp = Py_BuildValue("(OiiN)", p->tok->filename, t->lineno, col_number, loc);
    if (!tmp) {
        goto error;
    }
    value = PyTuple_Pack(2, errstr, tmp);
    Py_DECREF(tmp);
    if (!value) {
        goto error;
    }
    PyErr_SetObject(errtype, value);

    Py_DECREF(errstr);
    Py_DECREF(value);
    return NULL;

error:
    Py_XDECREF(errstr);
    Py_XDECREF(loc);
    return NULL;
}

void *_PyPegen_arguments_parsing_error(Parser *p, expr_ty e) {
    int kwarg_unpacking = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(e->v.Call.keywords); i < l; i++) {
        keyword_ty keyword = asdl_seq_GET(e->v.Call.keywords, i);
        if (!keyword->arg) {
            kwarg_unpacking = 1;
        }
    }

    const char *msg = NULL;
    if (kwarg_unpacking) {
        msg = "positional argument follows keyword argument unpacking";
    } else {
        msg = "positional argument follows keyword argument";
    }

    return RAISE_SYNTAX_ERROR(msg);
}

#if 0
static const char *
token_name(int type)
{
    if (0 <= type && type <= N_TOKENS) {
        return _PyParser_TokenNames[type];
    }
    return "<Huh?>";
}
#endif

// Here, mark is the start of the node, while p->mark is the end.
// If node==NULL, they should be the same.
int
_PyPegen_insert_memo(Parser *p, int mark, int type, void *node)
{
    // Insert in front
    Memo *m = PyArena_Malloc(p->arena, sizeof(Memo));
    if (m == NULL) {
        return -1;
    }
    m->type = type;
    m->node = node;
    m->mark = p->mark;
    m->next = p->tokens[mark]->memo;
    p->tokens[mark]->memo = m;
    return 0;
}

// Like _PyPegen_insert_memo(), but updates an existing node if found.
int
_PyPegen_update_memo(Parser *p, int mark, int type, void *node)
{
    for (Memo *m = p->tokens[mark]->memo; m != NULL; m = m->next) {
        if (m->type == type) {
            // Update existing node.
            m->node = node;
            m->mark = p->mark;
            return 0;
        }
    }
    // Insert new node.
    return _PyPegen_insert_memo(p, mark, type, node);
}

// Return dummy NAME.
void *
_PyPegen_dummy_name(Parser *p, ...)
{
    static void *cache = NULL;

    if (cache != NULL) {
        return cache;
    }

    PyObject *id = _create_dummy_identifier(p);
    if (!id) {
        return NULL;
    }
    cache = Name(id, Load, 1, 0, 1, 0, p->arena);
    return cache;
}

static int
_get_keyword_or_name_type(Parser *p, const char *name, int name_len)
{
    if (name_len >= p->n_keyword_lists || p->keywords[name_len] == NULL) {
        return NAME;
    }
    for (KeywordToken *k = p->keywords[name_len]; k->type != -1; k++) {
        if (strncmp(k->str, name, name_len) == 0) {
            return k->type;
        }
    }
    return NAME;
}

int
_PyPegen_fill_token(Parser *p)
{
    const char *start, *end;
    int type = PyTokenizer_Get(p->tok, &start, &end);
    if (type == ERRORTOKEN) {
        return tokenizer_error(p);
    }
    if (type == ENDMARKER && p->start_rule == Py_single_input && p->parsing_started) {
        type = NEWLINE; /* Add an extra newline */
        p->parsing_started = 0;

        if (p->tok->indent) {
            p->tok->pendin = -p->tok->indent;
            p->tok->indent = 0;
        }
    }
    else {
        p->parsing_started = 1;
    }

    if (p->fill == p->size) {
        int newsize = p->size * 2;
        p->tokens = PyMem_Realloc(p->tokens, newsize * sizeof(Token *));
        if (p->tokens == NULL) {
            PyErr_Format(PyExc_MemoryError, "Realloc tokens failed");
            return -1;
        }
        for (int i = p->size; i < newsize; i++) {
            p->tokens[i] = PyMem_Malloc(sizeof(Token));
            memset(p->tokens[i], '\0', sizeof(Token));
        }
        p->size = newsize;
    }

    Token *t = p->tokens[p->fill];
    t->type = (type == NAME) ? _get_keyword_or_name_type(p, start, (int)(end - start)) : type;
    t->bytes = PyBytes_FromStringAndSize(start, end - start);
    if (t->bytes == NULL) {
        return -1;
    }
    PyArena_AddPyObject(p->arena, t->bytes);

    int lineno = type == STRING ? p->tok->first_lineno : p->tok->lineno;
    const char *line_start = type == STRING ? p->tok->multi_line_start : p->tok->line_start;
    int end_lineno = p->tok->lineno;
    int col_offset = -1, end_col_offset = -1;
    if (start != NULL && start >= line_start) {
        col_offset = start - line_start;
    }
    if (end != NULL && end >= p->tok->line_start) {
        end_col_offset = end - p->tok->line_start;
    }

    t->lineno = p->starting_lineno + lineno;
    t->col_offset = p->tok->lineno == 1 ? p->starting_col_offset + col_offset : col_offset;
    t->end_lineno = p->starting_lineno + end_lineno;
    t->end_col_offset = p->tok->lineno == 1 ? p->starting_col_offset + end_col_offset : end_col_offset;

    // if (p->fill % 100 == 0) fprintf(stderr, "Filled at %d: %s \"%s\"\n", p->fill,
    // token_name(type), PyBytes_AsString(t->bytes));
    p->fill += 1;
    return 0;
}

// Instrumentation to count the effectiveness of memoization.
// The array counts the number of tokens skipped by memoization,
// indexed by type.

#define NSTATISTICS 2000
static long memo_statistics[NSTATISTICS];

void
_PyPegen_clear_memo_statistics()
{
    for (int i = 0; i < NSTATISTICS; i++) {
        memo_statistics[i] = 0;
    }
}

PyObject *
_PyPegen_get_memo_statistics()
{
    PyObject *ret = PyList_New(NSTATISTICS);
    if (ret == NULL) {
        return NULL;
    }
    for (int i = 0; i < NSTATISTICS; i++) {
        PyObject *value = PyLong_FromLong(memo_statistics[i]);
        if (value == NULL) {
            Py_DECREF(ret);
            return NULL;
        }
        // PyList_SetItem borrows a reference to value.
        if (PyList_SetItem(ret, i, value) < 0) {
            Py_DECREF(ret);
            return NULL;
        }
    }
    return ret;
}

int  // bool
_PyPegen_is_memoized(Parser *p, int type, void *pres)
{
    if (p->mark == p->fill) {
        if (_PyPegen_fill_token(p) < 0) {
            return -1;
        }
    }

    Token *t = p->tokens[p->mark];

    for (Memo *m = t->memo; m != NULL; m = m->next) {
        if (m->type == type) {
            if (0 <= type && type < NSTATISTICS) {
                long count = m->mark - p->mark;
                // A memoized negative result counts for one.
                if (count <= 0) {
                    count = 1;
                }
                memo_statistics[type] += count;
            }
            p->mark = m->mark;
            *(void **)(pres) = m->node;
            // fprintf(stderr, "%d < %d: memoized!\n", p->mark, p->fill);
            return 1;
        }
    }
    // fprintf(stderr, "%d < %d: not memoized\n", p->mark, p->fill);
    return 0;
}

int
_PyPegen_lookahead_with_string(int positive, void *(func)(Parser *, const char *), Parser *p,
                      const char *arg)
{
    int mark = p->mark;
    void *res = func(p, arg);
    p->mark = mark;
    return (res != NULL) == positive;
}

int
_PyPegen_lookahead_with_int(int positive, Token *(func)(Parser *, int), Parser *p, int arg)
{
    int mark = p->mark;
    void *res = func(p, arg);
    p->mark = mark;
    return (res != NULL) == positive;
}

int
_PyPegen_lookahead(int positive, void *(func)(Parser *), Parser *p)
{
    int mark = p->mark;
    void *res = func(p);
    p->mark = mark;
    return (res != NULL) == positive;
}

Token *
_PyPegen_expect_token(Parser *p, int type)
{
    if (p->mark == p->fill) {
        if (_PyPegen_fill_token(p) < 0) {
            return NULL;
        }
    }
    Token *t = p->tokens[p->mark];
    if (t->type != type) {
        // fprintf(stderr, "No %s at %d\n", token_name(type), p->mark);
        return NULL;
    }
    p->mark += 1;
    // fprintf(stderr, "Got %s at %d: %s\n", token_name(type), p->mark,
    // PyBytes_AsString(t->bytes));

    return t;
}

Token *
_PyPegen_get_last_nonnwhitespace_token(Parser *p)
{
    assert(p->mark >= 0);
    Token *token = NULL;
    for (int m = p->mark - 1; m >= 0; m--) {
        token = p->tokens[m];
        if (token->type != ENDMARKER && (token->type < NEWLINE || token->type > DEDENT)) {
            break;
        }
    }
    return token;
}

void *
_PyPegen_async_token(Parser *p)
{
    return _PyPegen_expect_token(p, ASYNC);
}

void *
_PyPegen_await_token(Parser *p)
{
    return _PyPegen_expect_token(p, AWAIT);
}

void *
_PyPegen_endmarker_token(Parser *p)
{
    return _PyPegen_expect_token(p, ENDMARKER);
}

expr_ty
_PyPegen_name_token(Parser *p)
{
    Token *t = _PyPegen_expect_token(p, NAME);
    if (t == NULL) {
        return NULL;
    }
    char* s = PyBytes_AsString(t->bytes);
    if (!s) {
        return NULL;
    }
    PyObject *id = _PyPegen_new_identifier(p, s);
    if (id == NULL) {
        return NULL;
    }
    return Name(id, Load, t->lineno, t->col_offset, t->end_lineno, t->end_col_offset,
                p->arena);
}

void *
_PyPegen_string_token(Parser *p)
{
    return _PyPegen_expect_token(p, STRING);
}

void *
_PyPegen_newline_token(Parser *p)
{
    return _PyPegen_expect_token(p, NEWLINE);
}

void *
_PyPegen_indent_token(Parser *p)
{
    return _PyPegen_expect_token(p, INDENT);
}

void *
_PyPegen_dedent_token(Parser *p)
{
    return _PyPegen_expect_token(p, DEDENT);
}

static PyObject *
parsenumber_raw(const char *s)
{
    const char *end;
    long x;
    double dx;
    Py_complex compl;
    int imflag;

    assert(s != NULL);
    errno = 0;
    end = s + strlen(s) - 1;
    imflag = *end == 'j' || *end == 'J';
    if (s[0] == '0') {
        x = (long)PyOS_strtoul(s, (char **)&end, 0);
        if (x < 0 && errno == 0) {
            return PyLong_FromString(s, (char **)0, 0);
        }
    }
    else
        x = PyOS_strtol(s, (char **)&end, 0);
    if (*end == '\0') {
        if (errno != 0)
            return PyLong_FromString(s, (char **)0, 0);
        return PyLong_FromLong(x);
    }
    /* XXX Huge floats may silently fail */
    if (imflag) {
        compl.real = 0.;
        compl.imag = PyOS_string_to_double(s, (char **)&end, NULL);
        if (compl.imag == -1.0 && PyErr_Occurred())
            return NULL;
        return PyComplex_FromCComplex(compl);
    }
    else {
        dx = PyOS_string_to_double(s, NULL, NULL);
        if (dx == -1.0 && PyErr_Occurred())
            return NULL;
        return PyFloat_FromDouble(dx);
    }
}

static PyObject *
parsenumber(const char *s)
{
    char *dup, *end;
    PyObject *res = NULL;

    assert(s != NULL);

    if (strchr(s, '_') == NULL) {
        return parsenumber_raw(s);
    }
    /* Create a duplicate without underscores. */
    dup = PyMem_Malloc(strlen(s) + 1);
    if (dup == NULL) {
        return PyErr_NoMemory();
    }
    end = dup;
    for (; *s; s++) {
        if (*s != '_') {
            *end++ = *s;
        }
    }
    *end = '\0';
    res = parsenumber_raw(dup);
    PyMem_Free(dup);
    return res;
}

expr_ty
_PyPegen_number_token(Parser *p)
{
    Token *t = _PyPegen_expect_token(p, NUMBER);
    if (t == NULL) {
        return NULL;
    }

    char *num_raw = PyBytes_AsString(t->bytes);

    if (num_raw == NULL) {
        return NULL;
    }

    PyObject *c = parsenumber(num_raw);

    if (c == NULL) {
        return NULL;
    }

    if (PyArena_AddPyObject(p->arena, c) < 0) {
        Py_DECREF(c);
        return NULL;
    }

    return Constant(c, NULL, t->lineno, t->col_offset, t->end_lineno, t->end_col_offset,
                    p->arena);
}

void
_PyPegen_Parser_Free(Parser *p)
{
    Py_XDECREF(p->normalize);
    for (int i = 0; i < p->size; i++) {
        PyMem_Free(p->tokens[i]);
    }
    PyMem_Free(p->tokens);
    PyMem_Free(p);
}

Parser *
_PyPegen_Parser_New(struct tok_state *tok, int start_rule, int *errcode, PyArena *arena)
{
    Parser *p = PyMem_Malloc(sizeof(Parser));
    if (p == NULL) {
        PyErr_Format(PyExc_MemoryError, "Out of memory for Parser");
        return NULL;
    }
    assert(tok != NULL);
    p->tok = tok;
    p->keywords = NULL;
    p->n_keyword_lists = -1;
    p->tokens = PyMem_Malloc(sizeof(Token *));
    if (!p->tokens) {
        PyMem_Free(p);
        PyErr_Format(PyExc_MemoryError, "Out of memory for tokens");
        return NULL;
    }
    p->tokens[0] = PyMem_Malloc(sizeof(Token));
    memset(p->tokens[0], '\0', sizeof(Token));
    p->mark = 0;
    p->fill = 0;
    p->size = 1;

    p->errcode = errcode;
    p->arena = arena;
    p->start_rule = start_rule;
    p->parsing_started = 0;
    p->normalize = NULL;
    p->error_indicator = 0;

    p->starting_lineno = 0;
    p->starting_col_offset = 0;

    return p;
}

void *
_PyPegen_run_parser(Parser *p)
{
    void *res = _PyPegen_parse(p);
    if (res == NULL) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        if (p->fill == 0) {
            RAISE_SYNTAX_ERROR("error at start before reading any input");
        }
        else if (p->tok->done == E_EOF) {
            RAISE_SYNTAX_ERROR("unexpected EOF while parsing");
        }
        else {
            if (p->tokens[p->fill-1]->type == INDENT) {
                RAISE_INDENTATION_ERROR("unexpected indent");
            }
            else if (p->tokens[p->fill-1]->type == DEDENT) {
                RAISE_INDENTATION_ERROR("unexpected unindent");
            }
            else {
                RAISE_SYNTAX_ERROR("invalid syntax");
            }
        }
        return NULL;
    }

    return res;
}

mod_ty
_PyPegen_run_parser_from_file_pointer(FILE *fp, int start_rule, PyObject *filename_ob,
                             const char *enc, const char *ps1, const char *ps2,
                             int *errcode, PyArena *arena)
{
    struct tok_state *tok = PyTokenizer_FromFile(fp, enc, ps1, ps2);
    if (tok == NULL) {
        if (PyErr_Occurred()) {
            raise_tokenizer_init_error(filename_ob);
            return NULL;
        }
        return NULL;
    }
    // This transfers the ownership to the tokenizer
    tok->filename = filename_ob;
    Py_INCREF(filename_ob);

    // From here on we need to clean up even if there's an error
    mod_ty result = NULL;

    Parser *p = _PyPegen_Parser_New(tok, start_rule, errcode, arena);
    if (p == NULL) {
        goto error;
    }

    result = _PyPegen_run_parser(p);
    _PyPegen_Parser_Free(p);

error:
    PyTokenizer_Free(tok);
    return result;
}

mod_ty
_PyPegen_run_parser_from_file(const char *filename, int start_rule,
                     PyObject *filename_ob, PyArena *arena)
{
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, filename);
        return NULL;
    }

    mod_ty result = _PyPegen_run_parser_from_file_pointer(fp, start_rule, filename_ob,
                                                 NULL, NULL, NULL, NULL, arena);

    fclose(fp);
    return result;
}

mod_ty
_PyPegen_run_parser_from_string(const char *str, int start_rule, PyObject *filename_ob,
                       int iflags, PyArena *arena)
{
    int exec_input = start_rule == Py_file_input;

    struct tok_state *tok;
    if (iflags & PyCF_IGNORE_COOKIE) {
        tok = PyTokenizer_FromUTF8(str, exec_input);
    } else {
        tok = PyTokenizer_FromString(str, exec_input);
    }
    if (tok == NULL) {
        if (PyErr_Occurred()) {
            raise_tokenizer_init_error(filename_ob);
        }
        return NULL;
    }
    // This transfers the ownership to the tokenizer
    tok->filename = filename_ob;
    Py_INCREF(filename_ob);

    // We need to clear up from here on
    mod_ty result = NULL;

    Parser *p = _PyPegen_Parser_New(tok, start_rule, NULL, arena);
    if (p == NULL) {
        goto error;
    }

    result = _PyPegen_run_parser(p);
    _PyPegen_Parser_Free(p);

error:
    PyTokenizer_Free(tok);
    return result;
}

void *
_PyPegen_interactive_exit(Parser *p)
{
    if (p->errcode) {
        *(p->errcode) = E_EOF;
    }
    return NULL;
}

/* Creates a single-element asdl_seq* that contains a */
asdl_seq *
_PyPegen_singleton_seq(Parser *p, void *a)
{
    assert(a != NULL);
    asdl_seq *seq = _Py_asdl_seq_new(1, p->arena);
    if (!seq) {
        return NULL;
    }
    asdl_seq_SET(seq, 0, a);
    return seq;
}

/* Creates a copy of seq and prepends a to it */
asdl_seq *
_PyPegen_seq_insert_in_front(Parser *p, void *a, asdl_seq *seq)
{
    assert(a != NULL);
    if (!seq) {
        return _PyPegen_singleton_seq(p, a);
    }

    asdl_seq *new_seq = _Py_asdl_seq_new(asdl_seq_LEN(seq) + 1, p->arena);
    if (!new_seq) {
        return NULL;
    }

    asdl_seq_SET(new_seq, 0, a);
    for (int i = 1, l = asdl_seq_LEN(new_seq); i < l; i++) {
        asdl_seq_SET(new_seq, i, asdl_seq_GET(seq, i - 1));
    }
    return new_seq;
}

static int
_get_flattened_seq_size(asdl_seq *seqs)
{
    int size = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seqs); i < l; i++) {
        asdl_seq *inner_seq = asdl_seq_GET(seqs, i);
        size += asdl_seq_LEN(inner_seq);
    }
    return size;
}

/* Flattens an asdl_seq* of asdl_seq*s */
asdl_seq *
_PyPegen_seq_flatten(Parser *p, asdl_seq *seqs)
{
    int flattened_seq_size = _get_flattened_seq_size(seqs);
    assert(flattened_seq_size > 0);

    asdl_seq *flattened_seq = _Py_asdl_seq_new(flattened_seq_size, p->arena);
    if (!flattened_seq) {
        return NULL;
    }

    int flattened_seq_idx = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seqs); i < l; i++) {
        asdl_seq *inner_seq = asdl_seq_GET(seqs, i);
        for (int j = 0, li = asdl_seq_LEN(inner_seq); j < li; j++) {
            asdl_seq_SET(flattened_seq, flattened_seq_idx++, asdl_seq_GET(inner_seq, j));
        }
    }
    assert(flattened_seq_idx == flattened_seq_size);

    return flattened_seq;
}

/* Creates a new name of the form <first_name>.<second_name> */
expr_ty
_PyPegen_join_names_with_dot(Parser *p, expr_ty first_name, expr_ty second_name)
{
    assert(first_name != NULL && second_name != NULL);
    PyObject *first_identifier = first_name->v.Name.id;
    PyObject *second_identifier = second_name->v.Name.id;

    if (PyUnicode_READY(first_identifier) == -1) {
        return NULL;
    }
    if (PyUnicode_READY(second_identifier) == -1) {
        return NULL;
    }
    const char *first_str = PyUnicode_AsUTF8(first_identifier);
    if (!first_str) {
        return NULL;
    }
    const char *second_str = PyUnicode_AsUTF8(second_identifier);
    if (!second_str) {
        return NULL;
    }
    ssize_t len = strlen(first_str) + strlen(second_str) + 1;  // +1 for the dot

    PyObject *str = PyBytes_FromStringAndSize(NULL, len);
    if (!str) {
        return NULL;
    }

    char *s = PyBytes_AS_STRING(str);
    if (!s) {
        return NULL;
    }

    strcpy(s, first_str);
    s += strlen(first_str);
    *s++ = '.';
    strcpy(s, second_str);
    s += strlen(second_str);
    *s = '\0';

    PyObject *uni = PyUnicode_DecodeUTF8(PyBytes_AS_STRING(str), PyBytes_GET_SIZE(str), NULL);
    Py_DECREF(str);
    if (!uni) {
        return NULL;
    }
    PyUnicode_InternInPlace(&uni);
    if (PyArena_AddPyObject(p->arena, uni) < 0) {
        Py_DECREF(uni);
        return NULL;
    }

    return _Py_Name(uni, Load, EXTRA_EXPR(first_name, second_name));
}

/* Counts the total number of dots in seq's tokens */
int
_PyPegen_seq_count_dots(asdl_seq *seq)
{
    int number_of_dots = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seq); i < l; i++) {
        Token *current_expr = asdl_seq_GET(seq, i);
        switch (current_expr->type) {
            case ELLIPSIS:
                number_of_dots += 3;
                break;
            case DOT:
                number_of_dots += 1;
                break;
            default:
                assert(current_expr->type == ELLIPSIS || current_expr->type == DOT);
        }
    }

    return number_of_dots;
}

/* Creates an alias with '*' as the identifier name */
alias_ty
_PyPegen_alias_for_star(Parser *p)
{
    PyObject *str = PyUnicode_InternFromString("*");
    if (!str) {
        return NULL;
    }
    if (PyArena_AddPyObject(p->arena, str) < 0) {
        Py_DECREF(str);
        return NULL;
    }
    return alias(str, NULL, p->arena);
}

/* Creates a new asdl_seq* with the identifiers of all the names in seq */
asdl_seq *
_PyPegen_map_names_to_ids(Parser *p, asdl_seq *seq)
{
    int len = asdl_seq_LEN(seq);
    assert(len > 0);

    asdl_seq *new_seq = _Py_asdl_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        expr_ty e = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, e->v.Name.id);
    }
    return new_seq;
}

/* Constructs a CmpopExprPair */
CmpopExprPair *
_PyPegen_cmpop_expr_pair(Parser *p, cmpop_ty cmpop, expr_ty expr)
{
    assert(expr != NULL);
    CmpopExprPair *a = PyArena_Malloc(p->arena, sizeof(CmpopExprPair));
    if (!a) {
        return NULL;
    }
    a->cmpop = cmpop;
    a->expr = expr;
    return a;
}

asdl_int_seq *
_PyPegen_get_cmpops(Parser *p, asdl_seq *seq)
{
    int len = asdl_seq_LEN(seq);
    assert(len > 0);

    asdl_int_seq *new_seq = _Py_asdl_int_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        CmpopExprPair *pair = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, pair->cmpop);
    }
    return new_seq;
}

asdl_seq *
_PyPegen_get_exprs(Parser *p, asdl_seq *seq)
{
    int len = asdl_seq_LEN(seq);
    assert(len > 0);

    asdl_seq *new_seq = _Py_asdl_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        CmpopExprPair *pair = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, pair->expr);
    }
    return new_seq;
}

/* Creates an asdl_seq* where all the elements have been changed to have ctx as context */
static asdl_seq *
_set_seq_context(Parser *p, asdl_seq *seq, expr_context_ty ctx)
{
    int len = asdl_seq_LEN(seq);
    if (len == 0) {
        return NULL;
    }

    asdl_seq *new_seq = _Py_asdl_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        expr_ty e = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, _PyPegen_set_expr_context(p, e, ctx));
    }
    return new_seq;
}

static expr_ty
_set_name_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _Py_Name(e->v.Name.id, ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_tuple_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _Py_Tuple(_set_seq_context(p, e->v.Tuple.elts, ctx), ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_list_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _Py_List(_set_seq_context(p, e->v.List.elts, ctx), ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_subscript_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _Py_Subscript(e->v.Subscript.value, e->v.Subscript.slice, ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_attribute_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _Py_Attribute(e->v.Attribute.value, e->v.Attribute.attr, ctx, EXTRA_EXPR(e, e));
}

static expr_ty
_set_starred_context(Parser *p, expr_ty e, expr_context_ty ctx)
{
    return _Py_Starred(_PyPegen_set_expr_context(p, e->v.Starred.value, ctx), ctx, EXTRA_EXPR(e, e));
}

/* Creates an `expr_ty` equivalent to `expr` but with `ctx` as context */
expr_ty
_PyPegen_set_expr_context(Parser *p, expr_ty expr, expr_context_ty ctx)
{
    assert(expr != NULL);

    expr_ty new = NULL;
    switch (expr->kind) {
        case Name_kind:
            new = _set_name_context(p, expr, ctx);
            break;
        case Tuple_kind:
            new = _set_tuple_context(p, expr, ctx);
            break;
        case List_kind:
            new = _set_list_context(p, expr, ctx);
            break;
        case Subscript_kind:
            new = _set_subscript_context(p, expr, ctx);
            break;
        case Attribute_kind:
            new = _set_attribute_context(p, expr, ctx);
            break;
        case Starred_kind:
            new = _set_starred_context(p, expr, ctx);
            break;
        default:
            new = expr;
    }
    return new;
}

/* Constructs a KeyValuePair that is used when parsing a dict's key value pairs */
KeyValuePair *
_PyPegen_key_value_pair(Parser *p, expr_ty key, expr_ty value)
{
    KeyValuePair *a = PyArena_Malloc(p->arena, sizeof(KeyValuePair));
    if (!a) {
        return NULL;
    }
    a->key = key;
    a->value = value;
    return a;
}

/* Extracts all keys from an asdl_seq* of KeyValuePair*'s */
asdl_seq *
_PyPegen_get_keys(Parser *p, asdl_seq *seq)
{
    int len = asdl_seq_LEN(seq);
    asdl_seq *new_seq = _Py_asdl_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        KeyValuePair *pair = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, pair->key);
    }
    return new_seq;
}

/* Extracts all values from an asdl_seq* of KeyValuePair*'s */
asdl_seq *
_PyPegen_get_values(Parser *p, asdl_seq *seq)
{
    int len = asdl_seq_LEN(seq);
    asdl_seq *new_seq = _Py_asdl_seq_new(len, p->arena);
    if (!new_seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        KeyValuePair *pair = asdl_seq_GET(seq, i);
        asdl_seq_SET(new_seq, i, pair->value);
    }
    return new_seq;
}

/* Constructs a NameDefaultPair */
NameDefaultPair *
_PyPegen_name_default_pair(Parser *p, arg_ty arg, expr_ty value)
{
    NameDefaultPair *a = PyArena_Malloc(p->arena, sizeof(NameDefaultPair));
    if (!a) {
        return NULL;
    }
    a->arg = arg;
    a->value = value;
    return a;
}

/* Constructs a SlashWithDefault */
SlashWithDefault *
_PyPegen_slash_with_default(Parser *p, asdl_seq *plain_names, asdl_seq *names_with_defaults)
{
    SlashWithDefault *a = PyArena_Malloc(p->arena, sizeof(SlashWithDefault));
    if (!a) {
        return NULL;
    }
    a->plain_names = plain_names;
    a->names_with_defaults = names_with_defaults;
    return a;
}

/* Constructs a StarEtc */
StarEtc *
_PyPegen_star_etc(Parser *p, arg_ty vararg, asdl_seq *kwonlyargs, arg_ty kwarg)
{
    StarEtc *a = PyArena_Malloc(p->arena, sizeof(StarEtc));
    if (!a) {
        return NULL;
    }
    a->vararg = vararg;
    a->kwonlyargs = kwonlyargs;
    a->kwarg = kwarg;
    return a;
}

asdl_seq *
_PyPegen_join_sequences(Parser *p, asdl_seq *a, asdl_seq *b)
{
    int first_len = asdl_seq_LEN(a);
    int second_len = asdl_seq_LEN(b);
    asdl_seq *new_seq = _Py_asdl_seq_new(first_len + second_len, p->arena);
    if (!new_seq) {
        return NULL;
    }

    int k = 0;
    for (Py_ssize_t i = 0; i < first_len; i++) {
        asdl_seq_SET(new_seq, k++, asdl_seq_GET(a, i));
    }
    for (Py_ssize_t i = 0; i < second_len; i++) {
        asdl_seq_SET(new_seq, k++, asdl_seq_GET(b, i));
    }

    return new_seq;
}

static asdl_seq *
_get_names(Parser *p, asdl_seq *names_with_defaults)
{
    int len = asdl_seq_LEN(names_with_defaults);
    asdl_seq *seq = _Py_asdl_seq_new(len, p->arena);
    if (!seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        NameDefaultPair *pair = asdl_seq_GET(names_with_defaults, i);
        asdl_seq_SET(seq, i, pair->arg);
    }
    return seq;
}

static asdl_seq *
_get_defaults(Parser *p, asdl_seq *names_with_defaults)
{
    int len = asdl_seq_LEN(names_with_defaults);
    asdl_seq *seq = _Py_asdl_seq_new(len, p->arena);
    if (!seq) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < len; i++) {
        NameDefaultPair *pair = asdl_seq_GET(names_with_defaults, i);
        asdl_seq_SET(seq, i, pair->value);
    }
    return seq;
}

/* Constructs an arguments_ty object out of all the parsed constructs in the parameters rule */
arguments_ty
_PyPegen_make_arguments(Parser *p, asdl_seq *slash_without_default,
                        SlashWithDefault *slash_with_default, asdl_seq *plain_names,
                        asdl_seq *names_with_default, StarEtc *star_etc)
{
    asdl_seq *posonlyargs;
    if (slash_without_default != NULL) {
        posonlyargs = slash_without_default;
    }
    else if (slash_with_default != NULL) {
        asdl_seq *slash_with_default_names =
            _get_names(p, slash_with_default->names_with_defaults);
        if (!slash_with_default_names) {
            return NULL;
        }
        posonlyargs = _PyPegen_join_sequences(p, slash_with_default->plain_names, slash_with_default_names);
        if (!posonlyargs) {
            return NULL;
        }
    }
    else {
        posonlyargs = _Py_asdl_seq_new(0, p->arena);
        if (!posonlyargs) {
            return NULL;
        }
    }

    asdl_seq *posargs;
    if (plain_names != NULL && names_with_default != NULL) {
        asdl_seq *names_with_default_names = _get_names(p, names_with_default);
        if (!names_with_default_names) {
            return NULL;
        }
        posargs = _PyPegen_join_sequences(p, plain_names, names_with_default_names);
        if (!posargs) {
            return NULL;
        }
    }
    else if (plain_names == NULL && names_with_default != NULL) {
        posargs = _get_names(p, names_with_default);
        if (!posargs) {
            return NULL;
        }
    }
    else if (plain_names != NULL && names_with_default == NULL) {
        posargs = plain_names;
    }
    else {
        posargs = _Py_asdl_seq_new(0, p->arena);
        if (!posargs) {
            return NULL;
        }
    }

    asdl_seq *posdefaults;
    if (slash_with_default != NULL && names_with_default != NULL) {
        asdl_seq *slash_with_default_values =
            _get_defaults(p, slash_with_default->names_with_defaults);
        if (!slash_with_default_values) {
            return NULL;
        }
        asdl_seq *names_with_default_values = _get_defaults(p, names_with_default);
        if (!names_with_default_values) {
            return NULL;
        }
        posdefaults = _PyPegen_join_sequences(p, slash_with_default_values, names_with_default_values);
        if (!posdefaults) {
            return NULL;
        }
    }
    else if (slash_with_default == NULL && names_with_default != NULL) {
        posdefaults = _get_defaults(p, names_with_default);
        if (!posdefaults) {
            return NULL;
        }
    }
    else if (slash_with_default != NULL && names_with_default == NULL) {
        posdefaults = _get_defaults(p, slash_with_default->names_with_defaults);
        if (!posdefaults) {
            return NULL;
        }
    }
    else {
        posdefaults = _Py_asdl_seq_new(0, p->arena);
        if (!posdefaults) {
            return NULL;
        }
    }

    arg_ty vararg = NULL;
    if (star_etc != NULL && star_etc->vararg != NULL) {
        vararg = star_etc->vararg;
    }

    asdl_seq *kwonlyargs;
    if (star_etc != NULL && star_etc->kwonlyargs != NULL) {
        kwonlyargs = _get_names(p, star_etc->kwonlyargs);
        if (!kwonlyargs) {
            return NULL;
        }
    }
    else {
        kwonlyargs = _Py_asdl_seq_new(0, p->arena);
        if (!kwonlyargs) {
            return NULL;
        }
    }

    asdl_seq *kwdefaults;
    if (star_etc != NULL && star_etc->kwonlyargs != NULL) {
        kwdefaults = _get_defaults(p, star_etc->kwonlyargs);
        if (!kwdefaults) {
            return NULL;
        }
    }
    else {
        kwdefaults = _Py_asdl_seq_new(0, p->arena);
        if (!kwdefaults) {
            return NULL;
        }
    }

    arg_ty kwarg = NULL;
    if (star_etc != NULL && star_etc->kwarg != NULL) {
        kwarg = star_etc->kwarg;
    }

    return _Py_arguments(posonlyargs, posargs, vararg, kwonlyargs, kwdefaults, kwarg,
                         posdefaults, p->arena);
}

/* Constructs an empty arguments_ty object, that gets used when a function accepts no
 * arguments. */
arguments_ty
_PyPegen_empty_arguments(Parser *p)
{
    asdl_seq *posonlyargs = _Py_asdl_seq_new(0, p->arena);
    if (!posonlyargs) {
        return NULL;
    }
    asdl_seq *posargs = _Py_asdl_seq_new(0, p->arena);
    if (!posargs) {
        return NULL;
    }
    asdl_seq *posdefaults = _Py_asdl_seq_new(0, p->arena);
    if (!posdefaults) {
        return NULL;
    }
    asdl_seq *kwonlyargs = _Py_asdl_seq_new(0, p->arena);
    if (!kwonlyargs) {
        return NULL;
    }
    asdl_seq *kwdefaults = _Py_asdl_seq_new(0, p->arena);
    if (!kwdefaults) {
        return NULL;
    }

    return _Py_arguments(posonlyargs, posargs, NULL, kwonlyargs, kwdefaults, NULL, kwdefaults,
                         p->arena);
}

/* Encapsulates the value of an operator_ty into an AugOperator struct */
AugOperator *
_PyPegen_augoperator(Parser *p, operator_ty kind)
{
    AugOperator *a = PyArena_Malloc(p->arena, sizeof(AugOperator));
    if (!a) {
        return NULL;
    }
    a->kind = kind;
    return a;
}

/* Construct a FunctionDef equivalent to function_def, but with decorators */
stmt_ty
_PyPegen_function_def_decorators(Parser *p, asdl_seq *decorators, stmt_ty function_def)
{
    assert(function_def != NULL);
    if (function_def->kind == AsyncFunctionDef_kind) {
        return _Py_AsyncFunctionDef(
            function_def->v.FunctionDef.name, function_def->v.FunctionDef.args,
            function_def->v.FunctionDef.body, decorators, function_def->v.FunctionDef.returns,
            function_def->v.FunctionDef.type_comment, function_def->lineno,
            function_def->col_offset, function_def->end_lineno, function_def->end_col_offset,
            p->arena);
    }

    return _Py_FunctionDef(function_def->v.FunctionDef.name, function_def->v.FunctionDef.args,
                           function_def->v.FunctionDef.body, decorators,
                           function_def->v.FunctionDef.returns,
                           function_def->v.FunctionDef.type_comment, function_def->lineno,
                           function_def->col_offset, function_def->end_lineno,
                           function_def->end_col_offset, p->arena);
}

/* Construct a ClassDef equivalent to class_def, but with decorators */
stmt_ty
_PyPegen_class_def_decorators(Parser *p, asdl_seq *decorators, stmt_ty class_def)
{
    assert(class_def != NULL);
    return _Py_ClassDef(class_def->v.ClassDef.name, class_def->v.ClassDef.bases,
                        class_def->v.ClassDef.keywords, class_def->v.ClassDef.body, decorators,
                        class_def->lineno, class_def->col_offset, class_def->end_lineno,
                        class_def->end_col_offset, p->arena);
}

/* Construct a KeywordOrStarred */
KeywordOrStarred *
_PyPegen_keyword_or_starred(Parser *p, void *element, int is_keyword)
{
    KeywordOrStarred *a = PyArena_Malloc(p->arena, sizeof(KeywordOrStarred));
    if (!a) {
        return NULL;
    }
    a->element = element;
    a->is_keyword = is_keyword;
    return a;
}

/* Get the number of starred expressions in an asdl_seq* of KeywordOrStarred*s */
static int
_seq_number_of_starred_exprs(asdl_seq *seq)
{
    int n = 0;
    for (Py_ssize_t i = 0, l = asdl_seq_LEN(seq); i < l; i++) {
        KeywordOrStarred *k = asdl_seq_GET(seq, i);
        if (!k->is_keyword) {
            n++;
        }
    }
    return n;
}

/* Extract the starred expressions of an asdl_seq* of KeywordOrStarred*s */
asdl_seq *
_PyPegen_seq_extract_starred_exprs(Parser *p, asdl_seq *kwargs)
{
    int new_len = _seq_number_of_starred_exprs(kwargs);
    if (new_len == 0) {
        return NULL;
    }
    asdl_seq *new_seq = _Py_asdl_seq_new(new_len, p->arena);
    if (!new_seq) {
        return NULL;
    }

    int idx = 0;
    for (Py_ssize_t i = 0, len = asdl_seq_LEN(kwargs); i < len; i++) {
        KeywordOrStarred *k = asdl_seq_GET(kwargs, i);
        if (!k->is_keyword) {
            asdl_seq_SET(new_seq, idx++, k->element);
        }
    }
    return new_seq;
}

/* Return a new asdl_seq* with only the keywords in kwargs */
asdl_seq *
_PyPegen_seq_delete_starred_exprs(Parser *p, asdl_seq *kwargs)
{
    int len = asdl_seq_LEN(kwargs);
    int new_len = len - _seq_number_of_starred_exprs(kwargs);
    if (new_len == 0) {
        return NULL;
    }
    asdl_seq *new_seq = _Py_asdl_seq_new(new_len, p->arena);
    if (!new_seq) {
        return NULL;
    }

    int idx = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        KeywordOrStarred *k = asdl_seq_GET(kwargs, i);
        if (k->is_keyword) {
            asdl_seq_SET(new_seq, idx++, k->element);
        }
    }
    return new_seq;
}

expr_ty
_PyPegen_concatenate_strings(Parser *p, asdl_seq *strings)
{
    int len = asdl_seq_LEN(strings);
    assert(len > 0);

    Token *first = asdl_seq_GET(strings, 0);
    Token *last = asdl_seq_GET(strings, len - 1);

    int bytesmode = 0;
    PyObject *bytes_str = NULL;

    FstringParser state;
    _PyPegen_FstringParser_Init(&state);

    for (Py_ssize_t i = 0; i < len; i++) {
        Token *t = asdl_seq_GET(strings, i);

        int this_bytesmode;
        int this_rawmode;
        PyObject *s;
        const char *fstr;
        Py_ssize_t fstrlen = -1;

        char *this_str = PyBytes_AsString(t->bytes);
        if (!this_str) {
            goto error;
        }

        if (_PyPegen_parsestr(p, this_str, &this_bytesmode, &this_rawmode, &s, &fstr, &fstrlen) != 0) {
            goto error;
        }

        /* Check that we are not mixing bytes with unicode. */
        if (i != 0 && bytesmode != this_bytesmode) {
            RAISE_SYNTAX_ERROR("cannot mix bytes and nonbytes literals");
            Py_XDECREF(s);
            goto error;
        }
        bytesmode = this_bytesmode;

        if (fstr != NULL) {
            assert(s == NULL && !bytesmode);

            int result = _PyPegen_FstringParser_ConcatFstring(p, &state, &fstr, fstr + fstrlen,
                                                     this_rawmode, 0, first, t, last);
            if (result < 0) {
                goto error;
            }
        }
        else {
            /* String or byte string. */
            assert(s != NULL && fstr == NULL);
            assert(bytesmode ? PyBytes_CheckExact(s) : PyUnicode_CheckExact(s));

            if (bytesmode) {
                if (i == 0) {
                    bytes_str = s;
                }
                else {
                    PyBytes_ConcatAndDel(&bytes_str, s);
                    if (!bytes_str) {
                        goto error;
                    }
                }
            }
            else {
                /* This is a regular string. Concatenate it. */
                if (_PyPegen_FstringParser_ConcatAndDel(&state, s) < 0) {
                    goto error;
                }
            }
        }
    }

    if (bytesmode) {
        if (PyArena_AddPyObject(p->arena, bytes_str) < 0) {
            goto error;
        }
        return Constant(bytes_str, NULL, first->lineno, first->col_offset, last->end_lineno,
                        last->end_col_offset, p->arena);
    }

    return _PyPegen_FstringParser_Finish(p, &state, first, last);

error:
    Py_XDECREF(bytes_str);
    _PyPegen_FstringParser_Dealloc(&state);
    if (PyErr_Occurred()) {
        raise_decode_error(p);
    }
    return NULL;
}
