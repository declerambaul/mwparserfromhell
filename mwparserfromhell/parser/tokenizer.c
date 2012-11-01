/*
Tokenizer for MWParserFromHell
Copyright (C) 2012 Ben Kurtovic <ben.kurtovic@verizon.net>

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "tokenizer.h"

static PyObject*
Tokenizer_new(PyTypeObject* type, PyObject* args, PyObject* kwds)
{
    Tokenizer* self = (Tokenizer*) type->tp_alloc(type, 0);
    return (PyObject*) self;
}

static struct Textbuffer*
Textbuffer_new(void)
{
    struct Textbuffer* buffer = PyObject_Malloc(sizeof(struct Textbuffer));
    if (!buffer) {
        PyErr_NoMemory();
        return NULL;
    }
    buffer->size = 0;
    buffer->data = PyObject_Malloc(sizeof(Py_UNICODE) * TEXTBUFFER_BLOCKSIZE);
    if (!buffer->data) {
        PyObject_Free(buffer);
        PyErr_NoMemory();
        return NULL;
    }
    buffer->next = NULL;
    return buffer;
}

static void
Tokenizer_dealloc(Tokenizer* self)
{
    Py_XDECREF(self->text);
    struct Stack *this = self->topstack, *next;
    while (this) {
        Py_DECREF(this->stack);
        Textbuffer_dealloc(this->textbuffer);
        next = this->next;
        PyObject_Free(this);
        this = next;
    }
    self->ob_type->tp_free((PyObject*) self);
}

static void
Textbuffer_dealloc(struct Textbuffer* this)
{
    struct Textbuffer* next;
    while (this) {
        PyObject_Free(this->data);
        next = this->next;
        PyObject_Free(this);
        this = next;
    }
}

static int
Tokenizer_init(Tokenizer* self, PyObject* args, PyObject* kwds)
{
    static char* kwlist[] = {NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "", kwlist))
        return -1;

    self->text = Py_None;
    Py_INCREF(Py_None);
    self->topstack = NULL;
    self->head = 0;
    self->length = 0;
    self->global = 0;
    return 0;
}

/*
    Add a new token stack, context, and textbuffer to the list.
*/
static int
Tokenizer_push(Tokenizer* self, int context)
{
    struct Stack* top = PyObject_Malloc(sizeof(struct Stack));
    if (!top) {
        PyErr_NoMemory();
        return -1;
    }
    top->stack = PyList_New(0);
    top->context = context;
    top->textbuffer = Textbuffer_new();
    if (!top->textbuffer) {
        return -1;
    }
    top->next = self->topstack;
    self->topstack = top;
    return 0;
}

/*
    Return the contents of the textbuffer as a Python Unicode object.
*/
static PyObject*
Textbuffer_render(struct Textbuffer* self)
{
    PyObject *result = PyUnicode_FromUnicode(self->data, self->size);
    PyObject *left, *concat;
    while (self->next) {
        self = self->next;
        left = PyUnicode_FromUnicode(self->data, self->size);
        concat = PyUnicode_Concat(left, result);
        Py_DECREF(left);
        Py_DECREF(result);
        result = concat;
    }
    return result;
}

/*
    Push the textbuffer onto the stack as a Text node and clear it.
*/
static int
Tokenizer_push_textbuffer(Tokenizer* self)
{
    struct Textbuffer* buffer = self->topstack->textbuffer;
    if (buffer->size == 0 && !buffer->next) {
        return 0;
    }
    PyObject* text = Textbuffer_render(buffer);
    if (!text) return -1;

    PyObject* kwargs = PyDict_New();
    if (!kwargs) {
        Py_DECREF(text);
        return -1;
    }
    PyDict_SetItemString(kwargs, "text", text);
    Py_DECREF(text);

    PyObject* token = PyObject_Call(Text, NOARGS, kwargs);
    Py_DECREF(kwargs);
    if (!token) return -1;

    if (PyList_Append(self->topstack->stack, token)) {
        Py_DECREF(token);
        return -1;
    }

    Py_DECREF(token);

    Textbuffer_dealloc(buffer);
    self->topstack->textbuffer = Textbuffer_new();
    if (!self->topstack->textbuffer) {
        return -1;
    }
    return 0;
}

static void
Tokenizer_delete_top_of_stack(Tokenizer* self)
{
    struct Stack* top = self->topstack;
    Py_DECREF(top->stack);
    Textbuffer_dealloc(top->textbuffer);
    self->topstack = top->next;
    PyObject_Free(top);
}

/*
    Pop the current stack/context/textbuffer, returing the stack.
*/
static PyObject*
Tokenizer_pop(Tokenizer* self)
{
    if (Tokenizer_push_textbuffer(self))
        return NULL;

    PyObject* stack = self->topstack->stack;
    Py_INCREF(stack);

    Tokenizer_delete_top_of_stack(self);
    return stack;
}

/*
    Pop the current stack/context/textbuffer, returing the stack. We will also
    replace the underlying stack's context with the current stack's.
*/
static PyObject*
Tokenizer_pop_keeping_context(Tokenizer* self)
{
    if (Tokenizer_push_textbuffer(self))
        return NULL;

    PyObject* stack = self->topstack->stack;
    Py_INCREF(stack);
    int context = self->topstack->context;

    Tokenizer_delete_top_of_stack(self);
    self->topstack->context = context;
    return stack;
}

/*
    Fail the current tokenization route. Discards the current
    stack/context/textbuffer and raises a BadRoute exception.
*/
static void*
Tokenizer_fail_route(Tokenizer* self)
{
    PyObject* stack = Tokenizer_pop(self);
    Py_XDECREF(stack);
    FAIL_ROUTE();
    return NULL;
}

/*
    Write a token to the end of the current token stack.
*/
static int
Tokenizer_write(Tokenizer* self, PyObject* token)
{
    if (Tokenizer_push_textbuffer(self))
        return -1;

    if (PyList_Append(self->topstack->stack, token))
        return -1;

    return 0;
}

/*
    Write a token to the beginning of the current token stack.
*/
static int
Tokenizer_write_first(Tokenizer* self, PyObject* token)
{
    if (Tokenizer_push_textbuffer(self))
        return -1;

    if (PyList_Insert(self->topstack->stack, 0, token))
        return -1;

    return 0;
}

/*
    Write text to the current textbuffer.
*/
static int
Tokenizer_write_text(Tokenizer* self, Py_UNICODE text)
{
    struct Textbuffer* buf = self->topstack->textbuffer;
    if (buf->size == TEXTBUFFER_BLOCKSIZE) {
        struct Textbuffer* new = Textbuffer_new();
        if (!new) {
            return -1;
        }
        new->next = buf;
        self->topstack->textbuffer = new;
        buf = new;
    }
    buf->data[buf->size] = text;
    buf->size++;
    return 0;
}

/*
    Write a series of tokens to the current stack at once.
*/
static int
Tokenizer_write_all(Tokenizer* self, PyObject* tokenlist)
{
    int pushed = 0;
    if (PyList_GET_SIZE(tokenlist) > 0) {
        PyObject* token = PyList_GET_ITEM(tokenlist, 0);
        switch (PyObject_IsInstance(token, Text)) {
            case 0:
                break;
            case 1: {
                pushed = 1;
                struct Textbuffer* buffer = self->topstack->textbuffer;
                if (buffer->size == 0 && !buffer->next) {
                    break;
                }
                PyObject* left = Textbuffer_render(buffer);
                if (!left) return -1;
                PyObject* right = PyObject_GetAttrString(token, "text");
                if (!right) return -1;

                PyObject* text = PyUnicode_Concat(left, right);
                Py_DECREF(left);
                Py_DECREF(right);
                if (!text) return -1;

                if (PyObject_SetAttrString(token, "text", text)) {
                    Py_DECREF(text);
                    return -1;
                }
                Py_DECREF(text);

                Textbuffer_dealloc(buffer);
                self->topstack->textbuffer = Textbuffer_new();
                if (!self->topstack->textbuffer) {
                    return -1;
                }
                break;
            }
            case -1:
                return -1;
        }
    }
    if (!pushed) {
        if (Tokenizer_push_textbuffer(self))
            return -1;
    }

    PyObject* stack = self->topstack->stack;
    Py_ssize_t size = PyList_GET_SIZE(stack);

    if (PyList_SetSlice(stack, size, size, tokenlist))
        return -1;

    return 0;
}

/*
    Pop the current stack, write text, and then write the stack. 'text' is a
    NULL-terminated array of chars.
*/
static int
Tokenizer_write_text_then_stack(Tokenizer* self, const char* text)
{
    PyObject* stack = Tokenizer_pop(self);
    int i = 0;
    while (1) {
        if (!text[i]) break;
        if (Tokenizer_write_text(self, (Py_UNICODE) text[i])) {
            Py_XDECREF(stack);
            return -1;
        }
        i++;
    }

    if (stack) {
        if (PyList_GET_SIZE(stack) > 0) {
            if (Tokenizer_write_all(self, stack)) {
                Py_DECREF(stack);
                return -1;
            }
        }
        Py_DECREF(stack);
    }

    self->head--;
    return 0;
}

/*
    Read the value at a relative point in the wikicode, forwards.
*/
static PyObject*
Tokenizer_read(Tokenizer* self, Py_ssize_t delta)
{
    Py_ssize_t index = self->head + delta;

    if (index >= self->length)
        return EMPTY;

    return PyList_GET_ITEM(self->text, index);
}

/*
    Read the value at a relative point in the wikicode, backwards.
*/
static PyObject*
Tokenizer_read_backwards(Tokenizer* self, Py_ssize_t delta)
{
    if (delta > self->head)
        return EMPTY;

    Py_ssize_t index = self->head - delta;
    return PyList_GET_ITEM(self->text, index);
}

/*
    Parse a template or argument at the head of the wikicode string.
*/
static int
Tokenizer_parse_template_or_argument(Tokenizer* self)
{
    self->head += 2;
    unsigned int braces = 2, i;

    while (*Tokenizer_READ(self, 0) == *"{") {
        self->head++;
        braces++;
    }
    if (Tokenizer_push(self, 0))
        return -1;

    while (braces) {
        if (braces == 1) {
            if (Tokenizer_write_text_then_stack(self, "{"))
                return -1;
            return 0;
        }

        if (braces == 2) {
            if (Tokenizer_parse_template(self))
                return -1;

            if (BAD_ROUTE) {
                RESET_ROUTE();
                if (Tokenizer_write_text_then_stack(self, "{{"))
                    return -1;
                return 0;
            }
            break;
        }

        if (Tokenizer_parse_argument(self))
            return -1;

        if (BAD_ROUTE) {
            RESET_ROUTE();
            if (Tokenizer_parse_template(self))
                return -1;

            if (BAD_ROUTE) {
                RESET_ROUTE();
                char text[braces + 1];
                for (i = 0; i < braces; i++) text[i] = *"{";
                text[braces] = *"";
                if (Tokenizer_write_text_then_stack(self, text)) {
                    Py_XDECREF(text);
                    return -1;
                }

                Py_XDECREF(text);
                return 0;
            }
            else {
                braces -= 2;
            }
        }
        else {
            braces -= 3;
        }

        if (braces) {
            self->head++;
        }
    }

    PyObject* tokenlist = Tokenizer_pop(self);
    if (!tokenlist) return -1;
    if (Tokenizer_write_all(self, tokenlist)) {
        Py_DECREF(tokenlist);
        return -1;
    }

    Py_DECREF(tokenlist);
    return 0;
}

/*
    Parse a template at the head of the wikicode string.
*/
static int
Tokenizer_parse_template(Tokenizer* self)
{
    PyObject *template, *token;
    Py_ssize_t reset = self->head;

    template = Tokenizer_parse(self, LC_TEMPLATE_NAME);
    if (BAD_ROUTE) {
        self->head = reset;
        return 0;
    }
    if (!template) return -1;

    token = PyObject_CallObject(TemplateOpen, NULL);
    if (!token) {
        Py_DECREF(template);
        return -1;
    }

    if (Tokenizer_write_first(self, token)) {
        Py_DECREF(token);
        Py_DECREF(template);
        return -1;
    }
    Py_DECREF(token);

    if (Tokenizer_write_all(self, template)) {
        Py_DECREF(template);
        return -1;
    }
    Py_DECREF(template);

    token = PyObject_CallObject(TemplateClose, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);

    return 0;
}

/*
    Parse an argument at the head of the wikicode string.
*/
static int
Tokenizer_parse_argument(Tokenizer* self)
{
    PyObject *argument, *token;
    Py_ssize_t reset = self->head;

    argument = Tokenizer_parse(self, LC_ARGUMENT_NAME);
    if (BAD_ROUTE) {
        self->head = reset;
        return 0;
    }
    if (!argument) return -1;

    token = PyObject_CallObject(ArgumentOpen, NULL);
    if (!token) {
        Py_DECREF(argument);
        return -1;
    }

    if (Tokenizer_write_first(self, token)) {
        Py_DECREF(token);
        Py_DECREF(argument);
        return -1;
    }
    Py_DECREF(token);

    if (Tokenizer_write_all(self, argument)) {
        Py_DECREF(argument);
        return -1;
    }
    Py_DECREF(argument);

    token = PyObject_CallObject(ArgumentClose, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);

    return 0;
}

/*
    Handle a template parameter at the head of the string.
*/
static int
Tokenizer_handle_template_param(Tokenizer* self)
{
    if (self->topstack->context & LC_TEMPLATE_NAME) {
        self->topstack->context ^= LC_TEMPLATE_NAME;
    }
    else if (self->topstack->context & LC_TEMPLATE_PARAM_VALUE) {
        self->topstack->context ^= LC_TEMPLATE_PARAM_VALUE;
    }

    if (self->topstack->context & LC_TEMPLATE_PARAM_KEY) {
        PyObject* stack = Tokenizer_pop_keeping_context(self);
        if (!stack) return -1;
        if (Tokenizer_write_all(self, stack)) {
            Py_DECREF(stack);
            return -1;
        }
        Py_DECREF(stack);
    }
    else {
        self->topstack->context |= LC_TEMPLATE_PARAM_KEY;
    }

    PyObject* token = PyObject_CallObject(TemplateParamSeparator, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);

    if (Tokenizer_push(self, self->topstack->context))
        return -1;
    return 0;
}

/*
    Handle a template parameter's value at the head of the string.
*/
static int
Tokenizer_handle_template_param_value(Tokenizer* self)
{
    PyObject* stack = Tokenizer_pop_keeping_context(self);
    if (!stack) return -1;
    if (Tokenizer_write_all(self, stack)) {
        Py_DECREF(stack);
        return -1;
    }
    Py_DECREF(stack);

    self->topstack->context ^= LC_TEMPLATE_PARAM_KEY;
    self->topstack->context |= LC_TEMPLATE_PARAM_VALUE;

    PyObject* token = PyObject_CallObject(TemplateParamEquals, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);
    return 0;
}

/*
    Handle the end of a template at the head of the string.
*/
static PyObject*
Tokenizer_handle_template_end(Tokenizer* self)
{
    PyObject* stack;
    if (self->topstack->context & LC_TEMPLATE_PARAM_KEY) {
        stack = Tokenizer_pop_keeping_context(self);
        if (!stack) return NULL;
        if (Tokenizer_write_all(self, stack)) {
            Py_DECREF(stack);
            return NULL;
        }
        Py_DECREF(stack);
    }

    self->head++;
    stack = Tokenizer_pop(self);
    return stack;
}

/*
    Handle the separator between an argument's name and default.
*/
static int
Tokenizer_handle_argument_separator(Tokenizer* self)
{
    self->topstack->context ^= LC_ARGUMENT_NAME;
    self->topstack->context |= LC_ARGUMENT_DEFAULT;

    PyObject* token = PyObject_CallObject(ArgumentSeparator, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);
    return 0;
}

/*
    Handle the end of an argument at the head of the string.
*/
static PyObject*
Tokenizer_handle_argument_end(Tokenizer* self)
{
    self->head += 2;
    PyObject* stack = Tokenizer_pop(self);
    return stack;
}

/*
    Parse an internal wikilink at the head of the wikicode string.
*/
static int
Tokenizer_parse_wikilink(Tokenizer* self)
{
    self->head += 2;
    Py_ssize_t reset = self->head - 1;

    PyObject *token;
    PyObject *wikilink = Tokenizer_parse(self, LC_WIKILINK_TITLE);

    if (BAD_ROUTE) {
        RESET_ROUTE();
        self->head = reset;
        int i;
        for (i = 0; i < 2; i++) {
            if (Tokenizer_write_text(self, *"["))
                return -1;
        }
        return 0;
    }
    if (!wikilink) return -1;

    token = PyObject_CallObject(WikilinkOpen, NULL);
    if (!token) {
        Py_DECREF(wikilink);
        return -1;
    }

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        Py_DECREF(wikilink);
        return -1;
    }
    Py_DECREF(token);

    if (Tokenizer_write_all(self, wikilink)) {
        Py_DECREF(wikilink);
        return -1;
    }
    Py_DECREF(wikilink);

    token = PyObject_CallObject(WikilinkClose, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);
    return 0;
}

/*
    Handle the separator between a wikilink's title and its text.
*/
static int
Tokenizer_handle_wikilink_separator(Tokenizer* self)
{
    self->topstack->context ^= LC_WIKILINK_TITLE;
    self->topstack->context |= LC_WIKILINK_TEXT;

    PyObject* token = PyObject_CallObject(WikilinkSeparator, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);
    return 0;
}

/*
    Handle the end of a wikilink at the head of the string.
*/
static PyObject*
Tokenizer_handle_wikilink_end(Tokenizer* self)
{
    self->head += 1;
    PyObject* stack = Tokenizer_pop(self);
    return stack;
}

/*
    Parse a section heading at the head of the wikicode string.
*/
static int
Tokenizer_parse_heading(Tokenizer* self)
{
    self->global |= GL_HEADING;
    Py_ssize_t reset = self->head;
    self->head += 1;
    int best = 1, i;

    while (*Tokenizer_READ(self, 0) == *"=") {
        best++;
        self->head++;
    }

    int context = LC_HEADING_LEVEL_1 << (best > 5 ? 5 : best - 1);
    HeadingData* heading = (HeadingData*) Tokenizer_parse(self, context);

    if (BAD_ROUTE) {
        RESET_ROUTE();
        self->head = reset + best - 1;
        char text[best + 1];
        for (i = 0; i < best; i++) text[i] = *"=";
        text[best] = *"";
        if (Tokenizer_write_text_then_stack(self, text))
            return -1;
        self->global ^= GL_HEADING;
        return 0;
    }

    PyObject* level = PyInt_FromSsize_t(heading->level);
    if (!level) {
        Py_DECREF(heading->title);
        PyObject_Free(heading);
        return -1;
    }

    PyObject* kwargs = PyDict_New();
    if (!kwargs) {
        Py_DECREF(level);
        Py_DECREF(heading->title);
        PyObject_Free(heading);
        return -1;
    }
    PyDict_SetItemString(kwargs, "level", level);
    Py_DECREF(level);

    PyObject* token = PyObject_Call(HeadingStart, NOARGS, kwargs);
    Py_DECREF(kwargs);
    if (!token) {
        Py_DECREF(heading->title);
        PyObject_Free(heading);
        return -1;
    }

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        Py_DECREF(heading->title);
        PyObject_Free(heading);
        return -1;
    }
    Py_DECREF(token);

    if (heading->level < best) {
        int diff = best - heading->level;
        char difftext[diff + 1];
        for (i = 0; i < diff; i++) difftext[i] = *"=";
        difftext[diff] = *"";
        if (Tokenizer_write_text_then_stack(self, difftext)) {
            Py_DECREF(heading->title);
            PyObject_Free(heading);
            return -1;
        }
    }

    if (Tokenizer_write_all(self, heading->title)) {
        Py_DECREF(heading->title);
        PyObject_Free(heading);
        return -1;
    }
    Py_DECREF(heading->title);
    PyObject_Free(heading);

    token = PyObject_CallObject(HeadingEnd, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);

    self->global ^= GL_HEADING;
    return 0;
}

/*
    Handle the end of a section heading at the head of the string.
*/
static HeadingData*
Tokenizer_handle_heading_end(Tokenizer* self)
{
    Py_ssize_t reset = self->head;
    self->head += 1;
    Py_ssize_t best = 1;
    int i;

    while (*Tokenizer_READ(self, 0) == *"=") {
        best++;
        self->head++;
    }

    int current = log2(self->topstack->context / LC_HEADING_LEVEL_1) + 1;
    int level = current > best ? (best > 6 ? 6 : best) : (current > 6 ? 6 : current);
    HeadingData* after = (HeadingData*) Tokenizer_parse(self, self->topstack->context);

    if (BAD_ROUTE) {
        RESET_ROUTE();
        if (level < best) {
            int diff = best - level;
            char difftext[diff + 1];
            for (i = 0; i < diff; i++) difftext[i] = *"=";
            difftext[diff] = *"";
            if (Tokenizer_write_text_then_stack(self, difftext))
                return NULL;
        }
        self->head = reset + best - 1;
    }

    else {
        char text[best + 1];
        for (i = 0; i < best; i++) text[i] = *"=";
        text[best] = *"";
        if (Tokenizer_write_text_then_stack(self, text)) {
            Py_DECREF(after->title);
            PyObject_Free(after);
            return NULL;
        }
        if (Tokenizer_write_all(self, after->title)) {
            Py_DECREF(after->title);
            PyObject_Free(after);
            return NULL;
        }
        Py_DECREF(after->title);
        level = after->level;
        PyObject_Free(after);
    }

    PyObject* stack = Tokenizer_pop(self);
    if (!stack) return NULL;

    HeadingData* heading = PyObject_Malloc(sizeof(HeadingData));
    if (!heading) {
        PyErr_NoMemory();
        return NULL;
    }
    heading->title = stack;
    heading->level = level;
    return heading;
}

/*
    Actually parse an HTML entity and ensure that it is valid.
*/
static int
Tokenizer_really_parse_entity(Tokenizer* self)
{
    return 0;
}

/*
    Parse an HTML entity at the head of the wikicode string.
*/
static int
Tokenizer_parse_entity(Tokenizer* self)
{
    Py_ssize_t reset = self->head;
    if (Tokenizer_push(self, 0))
        return -1;

    if (Tokenizer_really_parse_entity(self))
            return -1;

    if (BAD_ROUTE) {
        RESET_ROUTE();
        self->head = reset;
        if (Tokenizer_write_text(self, *PyUnicode_AS_UNICODE(Tokenizer_read(self, 0))))
            return -1;
        return 0;
    }

    PyObject* tokenlist = Tokenizer_pop(self);
    if (!tokenlist) return -1;
    if (Tokenizer_write_all(self, tokenlist)) {
        Py_DECREF(tokenlist);
        return -1;
    }

    Py_DECREF(tokenlist);
    return 0;
}

/*
    Parse an HTML comment at the head of the wikicode string.
*/
static int
Tokenizer_parse_comment(Tokenizer* self)
{
    self->head += 4;
    Py_ssize_t reset = self->head - 1;

    PyObject *token;
    PyObject *comment = Tokenizer_parse(self, LC_WIKILINK_TITLE);

    if (BAD_ROUTE) {
        RESET_ROUTE();
        self->head = reset;
        const char* text = "<!--";
        int i = 0;
        while (1) {
            if (!text[i]) return 0;
            if (Tokenizer_write_text(self, (Py_UNICODE) text[i])) {
                Py_XDECREF(text);
                return -1;
            }
            i++;
        }
        return 0;
    }
    if (!comment) return -1;

    token = PyObject_CallObject(CommentStart, NULL);
    if (!token) {
        Py_DECREF(comment);
        return -1;
    }

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        Py_DECREF(comment);
        return -1;
    }
    Py_DECREF(token);

    if (Tokenizer_write_all(self, comment)) {
        Py_DECREF(comment);
        return -1;
    }
    Py_DECREF(comment);

    token = PyObject_CallObject(CommentEnd, NULL);
    if (!token) return -1;

    if (Tokenizer_write(self, token)) {
        Py_DECREF(token);
        return -1;
    }
    Py_DECREF(token);
    self->head += 2;
    return 0;
}

/*
    Make sure we are not trying to write an invalid character.
*/
static void
Tokenizer_verify_safe(Tokenizer* self, int context, Py_UNICODE data)
{
    if (context & LC_FAIL_NEXT) {
        Tokenizer_fail_route(self);
        return;
    }

    if (context & (LC_TEMPLATE_NAME | LC_WIKILINK_TITLE)) {
        if (data == *"{" || data == *"}" || data == *"[" || data == *"]") {
            self->topstack->context |= LC_FAIL_NEXT;
            return;
        }
    }
    else if (context & (LC_TEMPLATE_PARAM_KEY | LC_ARGUMENT_NAME)) {
        if (context & LC_FAIL_ON_LBRACE) {
            if (data == *"{") {
                self->topstack->context |= LC_FAIL_NEXT;
                return;
            }
            self->topstack->context ^= LC_FAIL_ON_LBRACE;
        }
        else if (context & LC_FAIL_ON_RBRACE) {
            if (data == *"}") {
                self->topstack->context |= LC_FAIL_NEXT;
                return;
            }
            self->topstack->context ^= LC_FAIL_ON_RBRACE;
        }
        else if (data == *"{") {
            self->topstack->context |= LC_FAIL_ON_LBRACE;
        }
        else if (data == *"}") {
            self->topstack->context |= LC_FAIL_ON_RBRACE;
        }
    }

    if (context & LC_HAS_TEXT) {
        if (context & LC_FAIL_ON_TEXT) {
            if (!Py_UNICODE_ISSPACE(data)) {
                Tokenizer_fail_route(self);
                return;
            }
        }
        else {
            if (data == *"\n") {
                self->topstack->context |= LC_FAIL_ON_TEXT;
            }
        }
    }
    else if (!Py_UNICODE_ISSPACE(data)) {
        self->topstack->context |= LC_HAS_TEXT;
    }
}

/*
    Parse the wikicode string, using context for when to stop.
*/
static PyObject*
Tokenizer_parse(Tokenizer* self, int context)
{
    static int fail_contexts = LC_TEMPLATE | LC_ARGUMENT | LC_WIKILINK | LC_HEADING | LC_COMMENT;
    static int unsafe_contexts = LC_TEMPLATE_NAME | LC_WIKILINK_TITLE | LC_TEMPLATE_PARAM_KEY | LC_ARGUMENT_NAME;
    int this_context, is_marker, i;
    Py_UNICODE this_data, next, next_next, last;
    PyObject *this;

    if (Tokenizer_push(self, context))
        return NULL;

    while (1) {
        this = Tokenizer_read(self, 0);
        this_data = *PyUnicode_AS_UNICODE(this);
        this_context = self->topstack->context;

        if (this_context & unsafe_contexts) {
            Tokenizer_verify_safe(self, this_context, this_data);
            if (BAD_ROUTE)
                return NULL;
        }

        is_marker = 0;
        for (i = 0; i < NUM_MARKERS; i++) {
            if (*MARKERS[i] == this_data) {
                is_marker = 1;
                break;
            }
        }

        if (!is_marker) {
            Tokenizer_write_text(self, this_data);
            self->head++;
            continue;
        }

        if (this_data == *"") {
            if (this_context & LC_TEMPLATE_PARAM_KEY) {
                PyObject* trash = Tokenizer_pop(self);
                Py_XDECREF(trash);
            }
            if (this_context & fail_contexts) {
                return Tokenizer_fail_route(self);
            }
            return Tokenizer_pop(self);
        }

        next = *Tokenizer_READ(self, 1);

        if (this_context & LC_COMMENT) {
            if (this_data == next && next == *"-") {
                if (*Tokenizer_READ(self, 2) == *">") {
                    return Tokenizer_pop(self);
                }
            }
            Tokenizer_write_text(self, this_data);
        }
        else if (this_data == next && next == *"{") {
            if (Tokenizer_parse_template_or_argument(self))
                return NULL;
            if (self->topstack->context & LC_FAIL_NEXT)
                self->topstack->context ^= LC_FAIL_NEXT;
        }
        else if (this_data == *"|" && this_context & LC_TEMPLATE) {
            if (Tokenizer_handle_template_param(self))
                return NULL;
        }
        else if (this_data == *"=" && this_context & LC_TEMPLATE_PARAM_KEY) {
            if (Tokenizer_handle_template_param_value(self))
                return NULL;
        }
        else if (this_data == next && next == *"}" && this_context & LC_TEMPLATE) {
            return Tokenizer_handle_template_end(self);
        }
        else if (this_data == *"|" && this_context & LC_ARGUMENT_NAME) {
            if (Tokenizer_handle_argument_separator(self))
                return NULL;
        }
        else if (this_data == next && next == *"}" && this_context & LC_ARGUMENT) {
            if (*Tokenizer_READ(self, 2) == *"}") {
                return Tokenizer_handle_argument_end(self);
            }
            Tokenizer_write_text(self, this_data);
        }
        else if (this_data == next && next == *"[") {
            if (!(this_context & LC_WIKILINK_TITLE)) {
                if (Tokenizer_parse_wikilink(self))
                    return NULL;
                if (self->topstack->context & LC_FAIL_NEXT)
                    self->topstack->context ^= LC_FAIL_NEXT;
            }
            else {
                Tokenizer_write_text(self, this_data);
            }
        }
        else if (this_data == *"|" && this_context & LC_WIKILINK_TITLE) {
            if (Tokenizer_handle_wikilink_separator(self))
                return NULL;
        }
        else if (this_data == next && next == *"]" && this_context & LC_WIKILINK) {
            return Tokenizer_handle_wikilink_end(self);
        }
        else if (this_data == *"=" && !(self->global & GL_HEADING)) {
            last = *PyUnicode_AS_UNICODE(Tokenizer_read_backwards(self, 1));
            if (last == *"\n" || last == *"") {
                if (Tokenizer_parse_heading(self))
                    return NULL;
            }
            else {
                Tokenizer_write_text(self, this_data);
            }
        }
        else if (this_data == *"=" && this_context & LC_HEADING) {
            return (PyObject*) Tokenizer_handle_heading_end(self);
        }
        else if (this_data == *"\n" && this_context & LC_HEADING) {
            return Tokenizer_fail_route(self);
        }
        else if (this_data == *"&") {
            if (Tokenizer_parse_entity(self))
                return NULL;
        }
        else if (this_data == *"<" && next == *"!") {
            next_next = *Tokenizer_READ(self, 2);
            if (next_next == *Tokenizer_READ(self, 3) && next_next == *"-") {
                if (Tokenizer_parse_comment(self))
                    return NULL;
            }
            else {
                Tokenizer_write_text(self, this_data);
            }
        }
        else {
            Tokenizer_write_text(self, this_data);
        }

        self->head++;
    }
}

/*
    Build a list of tokens from a string of wikicode and return it.
*/
static PyObject*
Tokenizer_tokenize(Tokenizer* self, PyObject* args)
{
    PyObject* text;

    if (!PyArg_ParseTuple(args, "U", &text)) {
        /* Failed to parse a Unicode object; try a string instead. */
        PyErr_Clear();
        const char* encoded;
        Py_ssize_t size;

        if (!PyArg_ParseTuple(args, "s#", &encoded, &size)) {
            return NULL;
        }

        PyObject* temp;
        temp = PyUnicode_FromStringAndSize(encoded, size);
        if (!text)
            return NULL;

        Py_XDECREF(self->text);
        text = PySequence_Fast(temp, "expected a sequence");
        Py_XDECREF(temp);
        self->text = text;
    }
    else {
        Py_XDECREF(self->text);
        self->text = PySequence_Fast(text, "expected a sequence");
    }

    self->length = PyList_GET_SIZE(self->text);

    return Tokenizer_parse(self, 0);
}

PyMODINIT_FUNC
init_tokenizer(void)
{
    PyObject* module;

    TokenizerType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&TokenizerType) < 0)
        return;

    module = Py_InitModule("_tokenizer", module_methods);

    Py_INCREF(&TokenizerType);
    PyModule_AddObject(module, "CTokenizer", (PyObject*) &TokenizerType);

    EMPTY = PyUnicode_FromString("");
    NOARGS = PyTuple_New(0);

    char* name = "mwparserfromhell.parser";
    PyObject* globals = PyEval_GetGlobals();
    PyObject* locals = PyEval_GetLocals();
    PyObject* fromlist = PyList_New(1);
    if (!fromlist) return;
    PyObject* submodname = PyBytes_FromString("tokens");
    if (!submodname) {
        Py_DECREF(fromlist);
        return;
    }
    PyList_SET_ITEM(fromlist, 0, submodname);

    PyObject* tokmodule = PyImport_ImportModuleLevel(name, globals, locals, fromlist, 0);
    Py_DECREF(fromlist);
    if (!tokmodule) {
        return;
    }

    tokens = PyObject_GetAttrString(tokmodule, "tokens");
    Py_DECREF(tokmodule);

    Text = PyObject_GetAttrString(tokens, "Text");

    TemplateOpen = PyObject_GetAttrString(tokens, "TemplateOpen");
    TemplateParamSeparator = PyObject_GetAttrString(tokens, "TemplateParamSeparator");
    TemplateParamEquals = PyObject_GetAttrString(tokens, "TemplateParamEquals");
    TemplateClose = PyObject_GetAttrString(tokens, "TemplateClose");

    ArgumentOpen = PyObject_GetAttrString(tokens, "ArgumentOpen");
    ArgumentSeparator = PyObject_GetAttrString(tokens, "ArgumentSeparator");
    ArgumentClose = PyObject_GetAttrString(tokens, "ArgumentClose");

    WikilinkOpen = PyObject_GetAttrString(tokens, "WikilinkOpen");
    WikilinkSeparator = PyObject_GetAttrString(tokens, "WikilinkSeparator");
    WikilinkClose = PyObject_GetAttrString(tokens, "WikilinkClose");

    HTMLEntityStart = PyObject_GetAttrString(tokens, "HTMLEntityStart");
    HTMLEntityNumeric = PyObject_GetAttrString(tokens, "HTMLEntityNumeric");
    HTMLEntityHex = PyObject_GetAttrString(tokens, "HTMLEntityHex");
    HTMLEntityEnd = PyObject_GetAttrString(tokens, "HTMLEntityEnd");

    HeadingStart = PyObject_GetAttrString(tokens, "HeadingStart");
    HeadingEnd = PyObject_GetAttrString(tokens, "HeadingEnd");

    CommentStart = PyObject_GetAttrString(tokens, "CommentStart");
    CommentEnd = PyObject_GetAttrString(tokens, "CommentEnd");

    TagOpenOpen = PyObject_GetAttrString(tokens, "TagOpenOpen");
    TagAttrStart = PyObject_GetAttrString(tokens, "TagAttrStart");
    TagAttrEquals = PyObject_GetAttrString(tokens, "TagAttrEquals");
    TagAttrQuote = PyObject_GetAttrString(tokens, "TagAttrQuote");
    TagCloseOpen = PyObject_GetAttrString(tokens, "TagCloseOpen");
    TagCloseSelfclose = PyObject_GetAttrString(tokens, "TagCloseSelfclose");
    TagOpenClose = PyObject_GetAttrString(tokens, "TagOpenClose");
    TagCloseClose = PyObject_GetAttrString(tokens, "TagCloseClose");
}
