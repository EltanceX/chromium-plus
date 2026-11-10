// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "base/memory/raw_ptr_exclusion.h"
#include "components/python_runtime/python_dom_module.h"

namespace native_python {
namespace {

constexpr size_t kBatchCommands = 4096;
constexpr size_t kBatchBytes = 512 * 1024;
constexpr size_t kLiveBatches = 64;

struct State {
  RAW_PTR_EXCLUSION PyObject* dom;
  RAW_PTR_EXCLUSION PyTypeObject* context_type;
  RAW_PTR_EXCLUSION PyTypeObject* batch_type;
  RAW_PTR_EXCLUSION PyTypeObject* gradient_type;
  size_t live_batches;
};
struct Context {
  PyObject_HEAD CanvasHandle handle;
};
struct BatchData {
  std::vector<CanvasCommand> commands;
  size_t bytes = 0;
  uint32_t gradients = 0;
  bool sealed = false;
};
struct Batch {
  PyObject_HEAD RAW_PTR_EXCLUSION PyObject* owner;
  RAW_PTR_EXCLUSION BatchData* data;
};
struct Gradient {
  PyObject_HEAD RAW_PTR_EXCLUSION PyObject* batch;
  uint32_t index;
};

State* ModuleState(PyObject* m) {
  return static_cast<State*>(PyModule_GetState(m));
}
PyObject* Owner(PyObject* object) {
  return PyType_GetModule(Py_TYPE(object));
}
DomHost* Host(PyObject* module) {
  auto* s = ModuleState(module);
  if (!s || !s->dom) {
    PyErr_SetString(PyExc_ReferenceError, "Canvas module is inactive");
    return nullptr;
  }
  return ActiveDomHost(s->dom);
}
bool Number(PyObject* o, double* value) {
  if (!PyFloat_CheckExact(o) && !PyLong_CheckExact(o)) {
    PyErr_SetString(PyExc_TypeError, "Canvas numbers must be int or float");
    return false;
  }
  *value = PyFloat_AsDouble(o);
  if (PyErr_Occurred()) {
    return false;
  }
  if (!std::isfinite(*value) || std::abs(*value) > 1000000) {
    PyErr_SetString(PyExc_ValueError,
                    "Canvas numbers must be finite and bounded");
    return false;
  }
  return true;
}
bool Text(PyObject* o, std::string* text, size_t limit = 1024) {
  if (!PyUnicode_CheckExact(o)) {
    PyErr_SetString(PyExc_TypeError, "Canvas text must be str");
    return false;
  }
  if (PyUnicode_GetLength(o) > static_cast<Py_ssize_t>(limit)) {
    PyErr_SetString(PyExc_ValueError, "Canvas string is too long");
    return false;
  }
  Py_ssize_t size;
  const char* bytes = PyUnicode_AsUTF8AndSize(o, &size);
  if (!bytes) {
    return false;
  }
  if (size > static_cast<Py_ssize_t>(limit)) {
    PyErr_SetString(PyExc_ValueError, "Canvas UTF-8 string is too long");
    return false;
  }
  text->assign(bytes, size);
  return true;
}
bool HexColor(const std::string& text) {
  return (text.size() == 4 || text.size() == 5 || text.size() == 7 ||
          text.size() == 9) &&
         text[0] == '#' &&
         text.find_first_not_of("0123456789abcdefABCDEF", 1) ==
             std::string::npos;
}
bool ReadContext(PyObject* m, PyObject* o, CanvasHandle* handle) {
  if (!Py_IS_TYPE(o, ModuleState(m)->context_type)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's canvas.Context2D");
    return false;
  }
  *handle = reinterpret_cast<Context*>(o)->handle;
  return true;
}
PyObject* MakeContext(PyObject* m, const CanvasResult& r) {
  if (!CheckDomResult(r.status)) {
    return nullptr;
  }
  if (!r.handle.document || !r.handle.generation || !r.handle.id ||
      !r.handle.version) {
    PyErr_SetString(PyExc_ReferenceError, "Invalid Canvas context handle");
    return nullptr;
  }
  auto* type = ModuleState(m)->context_type;
  auto* o = reinterpret_cast<Context*>(type->tp_alloc(type, 0));
  if (o) {
    o->handle = r.handle;
  }
  return reinterpret_cast<PyObject*>(o);
}
bool Dimensions(PyObject* args, Py_ssize_t offset, uint32_t* w, uint32_t* h) {
  if (PyTuple_GET_SIZE(args) != offset + 2) {
    PyErr_SetString(PyExc_TypeError, "Expected width and height");
    return false;
  }
  std::array<uint32_t*, 2> outputs{w, h};
  for (int i = 0; i < 2; ++i) {
    auto* value = PyTuple_GetItem(args, offset + i);
    if (!PyLong_CheckExact(value)) {
      PyErr_SetString(PyExc_TypeError, "Canvas dimensions must be int");
      return false;
    }
    const long n = PyLong_AsLong(value);
    if (PyErr_Occurred()) {
      return false;
    }
    if (n < 1 || n > 2048) {
      PyErr_SetString(PyExc_ValueError,
                      "Canvas dimensions must be in [1, 2048]");
      return false;
    }
    *outputs[i] = static_cast<uint32_t>(n);
  }
  return true;
}
PyObject* GetContext(PyObject* m, PyObject* element) {
  if (!Host(m)) {
    return nullptr;
  }
  DomHandle handle;
  if (!ReadDomElementHandle(ModuleState(m)->dom, element, &handle)) {
    return nullptr;
  }
  auto* host = Host(m);
  return host ? MakeContext(m, host->GetCanvasContext(handle)) : nullptr;
}
PyObject* CreateSurface(PyObject* m, PyObject* args) {
  uint32_t w, h;
  if (!Dimensions(args, 0, &w, &h)) {
    return nullptr;
  }
  auto* host = Host(m);
  return host ? MakeContext(m, host->CreateCanvasSurface(w, h)) : nullptr;
}
PyObject* Resize(PyObject* m, PyObject* args) {
  uint32_t w, h;
  CanvasHandle handle;
  if (!Dimensions(args, 1, &w, &h) ||
      !ReadContext(m, PyTuple_GetItem(args, 0), &handle)) {
    return nullptr;
  }
  auto* host = Host(m);
  return host ? MakeContext(m, host->ResizeCanvas(handle, w, h)) : nullptr;
}
PyObject* Release(PyObject* m, PyObject* object) {
  CanvasHandle handle;
  if (!ReadContext(m, object, &handle)) {
    return nullptr;
  }
  auto* host = Host(m);
  if (!host || !CheckDomResult(host->ReleaseCanvas(handle))) {
    return nullptr;
  }
  Py_RETURN_NONE;
}
int TraverseContext(PyObject* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  return 0;
}
void DeleteContext(PyObject* self) {
  PyObject_GC_UnTrack(self);
  auto* type = Py_TYPE(self);
  type->tp_free(self);
  Py_DECREF(type);
}
int TraverseBatch(PyObject* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  Py_VISIT(reinterpret_cast<Batch*>(self)->owner);
  return 0;
}
int ClearBatch(PyObject* self) {
  auto* b = reinterpret_cast<Batch*>(self);
  if (b->owner) {
    --ModuleState(b->owner)->live_batches;
    Py_CLEAR(b->owner);
  }
  delete std::exchange(b->data, nullptr);
  return 0;
}
void DeleteBatch(PyObject* self) {
  PyObject_GC_UnTrack(self);
  ClearBatch(self);
  auto* type = Py_TYPE(self);
  type->tp_free(self);
  Py_DECREF(type);
}
int TraverseGradient(PyObject* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  Py_VISIT(reinterpret_cast<Gradient*>(self)->batch);
  return 0;
}
int ClearGradient(PyObject* self) {
  Py_CLEAR(reinterpret_cast<Gradient*>(self)->batch);
  return 0;
}
void DeleteGradient(PyObject* self) {
  PyObject_GC_UnTrack(self);
  ClearGradient(self);
  auto* type = Py_TYPE(self);
  type->tp_free(self);
  Py_DECREF(type);
}
BatchData* Mutable(PyObject* self) {
  auto* b = reinterpret_cast<Batch*>(self);
  if (!b->owner || !b->data || !Host(b->owner)) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_ReferenceError, "Canvas batch is inactive");
    }
    return nullptr;
  }
  if (b->data->sealed) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Submitted Canvas batches are immutable");
    return nullptr;
  }
  return b->data;
}
PyObject* Append(PyObject* self, CanvasCommand command) {
  auto* data = Mutable(self);
  if (!data) {
    return nullptr;
  }
  const size_t bytes = 128 + command.text.size() + command.values.size() * 8;
  if (data->commands.size() == kBatchCommands ||
      bytes > kBatchBytes - data->bytes) {
    PyErr_SetString(PyExc_ValueError,
                    "Canvas batch exceeds 4096 commands or 512 KiB");
    return nullptr;
  }
  data->bytes += bytes;
  data->commands.push_back(std::move(command));
  Py_RETURN_NONE;
}
PyObject* Numeric(PyObject* self,
                  PyObject* args,
                  CanvasOp op,
                  int n,
                  bool boolean = false) {
  const auto size = PyTuple_GET_SIZE(args);
  if (size != n && !(boolean && size == n + 1)) {
    PyErr_SetString(PyExc_TypeError, "Incorrect Canvas argument count");
    return nullptr;
  }
  CanvasCommand c;
  c.op = op;
  for (int i = 0; i < n; ++i) {
    if (!Number(PyTuple_GetItem(args, i), &c.args[i])) {
      return nullptr;
    }
  }
  if (boolean && size == n + 1) {
    auto* value = PyTuple_GetItem(args, n);
    if (!PyBool_Check(value)) {
      PyErr_SetString(PyExc_TypeError, "Expected bool");
      return nullptr;
    }
    c.args[n] = value == Py_True;
  }
  return Append(self, std::move(c));
}
#define NUMERIC(name, op, count)                  \
  PyObject* name(PyObject* s, PyObject* a) {      \
    return Numeric(s, a, CanvasOp::k##op, count); \
  }
NUMERIC(ClearRect, ClearRect, 4)
NUMERIC(FillRect, FillRect, 4)
NUMERIC(StrokeRect, StrokeRect, 4)
NUMERIC(BeginPath, BeginPath, 0)
NUMERIC(ClosePath, ClosePath, 0)
NUMERIC(MoveTo, MoveTo, 2)
NUMERIC(LineTo, LineTo, 2)
NUMERIC(Fill, Fill, 0)
NUMERIC(Stroke, Stroke, 0)
NUMERIC(Save, Save, 0)
NUMERIC(Restore, Restore, 0)
NUMERIC(Translate, Translate, 2)
NUMERIC(Scale, Scale, 2)
NUMERIC(Rotate, Rotate, 1)
NUMERIC(LineWidth, LineWidth, 1)
NUMERIC(Alpha, Alpha, 1)
#undef NUMERIC
PyObject* Arc(PyObject* s, PyObject* a) {
  return Numeric(s, a, CanvasOp::kArc, 5, true);
}
PyObject* Ellipse(PyObject* s, PyObject* a) {
  return Numeric(s, a, CanvasOp::kEllipse, 7, true);
}
PyObject* Style(PyObject* self, PyObject* value, bool stroke) {
  CanvasCommand c;
  auto* m = Owner(self);
  if (Py_IS_TYPE(value, ModuleState(m)->gradient_type)) {
    auto* gradient = reinterpret_cast<Gradient*>(value);
    if (gradient->batch != self) {
      PyErr_SetString(PyExc_ValueError, "Gradient belongs to another batch");
      return nullptr;
    }
    c.op = stroke ? CanvasOp::kStrokeGradient : CanvasOp::kFillGradient;
    c.index = gradient->index;
  } else {
    c.op = stroke ? CanvasOp::kStrokeColor : CanvasOp::kFillColor;
    if (!Text(value, &c.text, 9)) {
      return nullptr;
    }
    if (!HexColor(c.text)) {
      PyErr_SetString(
          PyExc_ValueError,
          "Canvas colors must be #RGB, #RGBA, #RRGGBB or #RRGGBBAA");
      return nullptr;
    }
  }
  return Append(self, std::move(c));
}
PyObject* FillStyle(PyObject* s, PyObject* v) {
  return Style(s, v, false);
}
PyObject* StrokeStyle(PyObject* s, PyObject* v) {
  return Style(s, v, true);
}
PyObject* LinearGradient(PyObject* self, PyObject* args) {
  auto* data = Mutable(self);
  if (!data) {
    return nullptr;
  }
  if (data->gradients == 32) {
    PyErr_SetString(PyExc_ValueError, "At most 32 gradients per batch");
    return nullptr;
  }
  auto* type = ModuleState(Owner(self))->gradient_type;
  auto* g = reinterpret_cast<Gradient*>(type->tp_alloc(type, 0));
  if (!g) {
    return nullptr;
  }
  PyObject* accepted = Numeric(self, args, CanvasOp::kLinearGradient, 4);
  if (!accepted) {
    Py_DECREF(g);
    return nullptr;
  }
  Py_DECREF(accepted);
  g->batch = Py_NewRef(self);
  g->index = ++data->gradients;
  data->commands.back().index = g->index;
  return reinterpret_cast<PyObject*>(g);
}
PyObject* ColorStop(PyObject* self, PyObject* args) {
  auto* g = reinterpret_cast<Gradient*>(self);
  if (!g->batch) {
    PyErr_SetString(PyExc_ReferenceError, "Gradient is inactive");
    return nullptr;
  }
  PyObject *offset, *color;
  if (!PyArg_ParseTuple(args, "OO", &offset, &color)) {
    return nullptr;
  }
  CanvasCommand c;
  c.op = CanvasOp::kColorStop;
  c.index = g->index;
  if (!Number(offset, &c.args[0]) || !Text(color, &c.text, 9)) {
    return nullptr;
  }
  if (c.args[0] < 0 || c.args[0] > 1 || !HexColor(c.text)) {
    PyErr_SetString(PyExc_ValueError, "Invalid gradient stop");
    return nullptr;
  }
  return Append(g->batch, std::move(c));
}
PyObject* LineDash(PyObject* self, PyObject* values) {
  if ((!PyList_CheckExact(values) && !PyTuple_CheckExact(values)) ||
      PySequence_Size(values) > 16) {
    PyErr_SetString(PyExc_TypeError,
                    "Line dash requires a list or tuple of at most 16 numbers");
    return nullptr;
  }
  CanvasCommand c;
  c.op = CanvasOp::kLineDash;
  for (Py_ssize_t i = 0; i < PySequence_Size(values); ++i) {
    double n;
    auto* o = PyList_CheckExact(values) ? PyList_GetItem(values, i)
                                        : PyTuple_GetItem(values, i);
    if (!Number(o, &n)) {
      return nullptr;
    }
    c.values.push_back(n);
  }
  return Append(self, std::move(c));
}
PyObject* StringCommand(PyObject* self, PyObject* value, CanvasOp op) {
  CanvasCommand c;
  c.op = op;
  return Text(value, &c.text) ? Append(self, std::move(c)) : nullptr;
}
PyObject* TextAlign(PyObject* s, PyObject* v) {
  return StringCommand(s, v, CanvasOp::kTextAlign);
}
PyObject* TextBaseline(PyObject* s, PyObject* v) {
  return StringCommand(s, v, CanvasOp::kTextBaseline);
}
PyObject* Font(PyObject* self, PyObject* args) {
  PyObject *size, *family = nullptr, *bold = Py_False;
  if (!PyArg_ParseTuple(args, "O|OO", &size, &family, &bold)) {
    return nullptr;
  }
  CanvasCommand c;
  c.op = CanvasOp::kFont;
  c.text = "monospace";
  if (!Number(size, &c.args[0]) || (family && !Text(family, &c.text, 32))) {
    return nullptr;
  }
  if (!PyBool_Check(bold)) {
    PyErr_SetString(PyExc_TypeError, "Font bold must be bool");
    return nullptr;
  }
  c.args[1] = bold == Py_True;
  return Append(self, std::move(c));
}
PyObject* FillText(PyObject* self, PyObject* args) {
  PyObject *text, *x, *y;
  if (!PyArg_ParseTuple(args, "OOO", &text, &x, &y)) {
    return nullptr;
  }
  CanvasCommand c;
  c.op = CanvasOp::kFillText;
  if (!Text(text, &c.text) || !Number(x, &c.args[0]) ||
      !Number(y, &c.args[1])) {
    return nullptr;
  }
  return Append(self, std::move(c));
}
PyObject* Smoothing(PyObject* self, PyObject* value) {
  if (!PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "Smoothing must be bool");
    return nullptr;
  }
  CanvasCommand c;
  c.op = CanvasOp::kSmoothing;
  c.args[0] = value == Py_True;
  return Append(self, std::move(c));
}
PyObject* DrawSurface(PyObject* self, PyObject* args) {
  if (PyTuple_GET_SIZE(args) != 5) {
    PyErr_SetString(PyExc_TypeError,
                    "draw_surface requires context, x, y, width, height");
    return nullptr;
  }
  CanvasCommand c;
  c.op = CanvasOp::kDrawSurface;
  if (!ReadContext(Owner(self), PyTuple_GetItem(args, 0), &c.source)) {
    return nullptr;
  }
  for (int i = 0; i < 4; ++i) {
    if (!Number(PyTuple_GetItem(args, i + 1), &c.args[i])) {
      return nullptr;
    }
  }
  return Append(self, std::move(c));
}
PyObject* NewBatch(PyObject* m, PyObject*) {
  if (!Host(m)) {
    return nullptr;
  }
  auto* state = ModuleState(m);
  if (state->live_batches == kLiveBatches) {
    PyErr_SetString(PyExc_ValueError, "At most 64 live Canvas batches");
    return nullptr;
  }
  auto* type = state->batch_type;
  auto* b = reinterpret_cast<Batch*>(type->tp_alloc(type, 0));
  if (!b) {
    return nullptr;
  }
  b->owner = Py_NewRef(m);
  ++state->live_batches;
  b->data = new BatchData;
  return reinterpret_cast<PyObject*>(b);
}
PyObject* Submit(PyObject* m, PyObject* args) {
  PyObject *context, *object;
  if (!PyArg_ParseTuple(args, "OO", &context, &object)) {
    return nullptr;
  }
  CanvasHandle handle;
  if (!ReadContext(m, context, &handle)) {
    return nullptr;
  }
  if (!Py_IS_TYPE(object, ModuleState(m)->batch_type)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's canvas.Batch");
    return nullptr;
  }
  auto* data = reinterpret_cast<Batch*>(object)->data;
  if (!data) {
    PyErr_SetString(PyExc_ReferenceError, "Batch is inactive");
    return nullptr;
  }
  auto* host = Host(m);
  if (!host) {
    return nullptr;
  }
  data->sealed = true;
  const auto r = host->SubmitCanvas(handle, data->commands);
  if (!CheckDomResult(r.status)) {
    return nullptr;
  }
  return Py_BuildValue("{sK,sK,sK,sd}", "commands", r.statistics.commands,
                       "bytes", r.statistics.bytes, "pixels",
                       r.statistics.pixels, "native_ms",
                       r.statistics.native_ms);
}
PyMethodDef kBatchMethods[] = {
    {"clear_rect", ClearRect, METH_VARARGS, nullptr},
    {"fill_rect", FillRect, METH_VARARGS, nullptr},
    {"stroke_rect", StrokeRect, METH_VARARGS, nullptr},
    {"begin_path", BeginPath, METH_VARARGS, nullptr},
    {"close_path", ClosePath, METH_VARARGS, nullptr},
    {"move_to", MoveTo, METH_VARARGS, nullptr},
    {"line_to", LineTo, METH_VARARGS, nullptr},
    {"arc", Arc, METH_VARARGS, nullptr},
    {"ellipse", Ellipse, METH_VARARGS, nullptr},
    {"fill", Fill, METH_VARARGS, nullptr},
    {"stroke", Stroke, METH_VARARGS, nullptr},
    {"save", Save, METH_VARARGS, nullptr},
    {"restore", Restore, METH_VARARGS, nullptr},
    {"translate", Translate, METH_VARARGS, nullptr},
    {"scale", Scale, METH_VARARGS, nullptr},
    {"rotate", Rotate, METH_VARARGS, nullptr},
    {"fill_style", FillStyle, METH_O, nullptr},
    {"stroke_style", StrokeStyle, METH_O, nullptr},
    {"linear_gradient", LinearGradient, METH_VARARGS, nullptr},
    {"line_width", LineWidth, METH_VARARGS, nullptr},
    {"line_dash", LineDash, METH_O, nullptr},
    {"alpha", Alpha, METH_VARARGS, nullptr},
    {"font", Font, METH_VARARGS, nullptr},
    {"text_align", TextAlign, METH_O, nullptr},
    {"text_baseline", TextBaseline, METH_O, nullptr},
    {"fill_text", FillText, METH_VARARGS, nullptr},
    {"smoothing", Smoothing, METH_O, nullptr},
    {"draw_surface", DrawSurface, METH_VARARGS, nullptr},
    {nullptr}};
PyMethodDef kGradientMethods[] = {
    {"add_color_stop", ColorStop, METH_VARARGS, nullptr},
    {nullptr}};
PyType_Slot kContextSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteContext)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseContext)},
    {0, nullptr}};
PyType_Slot kBatchSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteBatch)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseBatch)},
    {Py_tp_clear, reinterpret_cast<void*>(ClearBatch)},
    {Py_tp_methods, kBatchMethods},
    {0, nullptr}};
PyType_Slot kGradientSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteGradient)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseGradient)},
    {Py_tp_clear, reinterpret_cast<void*>(ClearGradient)},
    {Py_tp_methods, kGradientMethods},
    {0, nullptr}};
constexpr unsigned kFlags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                            Py_TPFLAGS_IMMUTABLETYPE |
                            Py_TPFLAGS_DISALLOW_INSTANTIATION;
PyType_Spec kContextSpec = {"canvas.Context2D", sizeof(Context), 0, kFlags,
                            kContextSlots};
PyType_Spec kBatchSpec = {"canvas.Batch", sizeof(Batch), 0, kFlags,
                          kBatchSlots};
PyType_Spec kGradientSpec = {"canvas.Gradient", sizeof(Gradient), 0, kFlags,
                             kGradientSlots};
int Traverse(PyObject* m, visitproc visit, void* arg) {
  auto* s = ModuleState(m);
  Py_VISIT(s->dom);
  Py_VISIT(s->context_type);
  Py_VISIT(s->batch_type);
  Py_VISIT(s->gradient_type);
  return 0;
}
int Clear(PyObject* m) {
  auto* s = ModuleState(m);
  Py_CLEAR(s->dom);
  Py_CLEAR(s->context_type);
  Py_CLEAR(s->batch_type);
  Py_CLEAR(s->gradient_type);
  return 0;
}
int Initialize(PyObject* m) {
  auto* s = ModuleState(m);
  s->dom = PyImport_ImportModule("dom");
  auto add = [&](PyTypeObject*& type, PyType_Spec& spec, const char* name) {
    type = reinterpret_cast<PyTypeObject*>(
        PyType_FromModuleAndSpec(m, &spec, nullptr));
    return type && PyModule_AddObjectRef(
                       m, name, reinterpret_cast<PyObject*>(type)) == 0;
  };
  return s->dom && add(s->context_type, kContextSpec, "Context2D") &&
                 add(s->batch_type, kBatchSpec, "Batch") &&
                 add(s->gradient_type, kGradientSpec, "Gradient")
             ? 0
             : -1;
}
PyMethodDef kMethods[] = {
    {"get_context_2d", GetContext, METH_O, nullptr},
    {"create_surface", CreateSurface, METH_VARARGS, nullptr},
    {"resize", Resize, METH_VARARGS, nullptr},
    {"release", Release, METH_O, nullptr},
    {"batch", NewBatch, METH_NOARGS, nullptr},
    {"submit", Submit, METH_VARARGS, nullptr},
    {nullptr}};
PyModuleDef_Slot kSlots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(Initialize)},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {0, nullptr}};
PyModuleDef kModule = {PyModuleDef_HEAD_INIT,
                       "canvas",
                       nullptr,
                       sizeof(State),
                       kMethods,
                       kSlots,
                       Traverse,
                       Clear,
                       nullptr};
}  // namespace
PyMODINIT_FUNC InitCanvasModule() {
  return PyModuleDef_Init(&kModule);
}
}  // namespace native_python
