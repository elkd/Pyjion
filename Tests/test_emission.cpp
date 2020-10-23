/*
* The MIT License (MIT)
*
* Copyright (c) Microsoft Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
*/

/**
  Test JIT code emission.
*/

#include "stdafx.h"
#include <catch2/catch.hpp>
#include "testing_util.h"
#include <Python.h>
#include <frameobject.h>
#include <util.h>
#include <pyjit.h>

class EmissionTest {
private:
    py_ptr<PyCodeObject> m_code;
    py_ptr<PyjionJittedCode> m_jittedcode;

    PyObject* run() {
        auto sysModule = PyObject_ptr(PyImport_ImportModule("sys"));
        auto globals = PyObject_ptr(PyDict_New());
        auto builtins = PyEval_GetBuiltins();
        PyDict_SetItemString(globals.get(), "__builtins__", builtins);
        PyDict_SetItemString(globals.get(), "sys", sysModule.get());

        // Don't DECREF as frames are recycled.
        auto frame = PyFrame_New(PyThreadState_Get(), m_code.get(), globals.get(), PyObject_ptr(PyDict_New()).get());

        auto res = m_jittedcode->j_evalfunc(m_jittedcode.get(), frame);
        REQUIRE(!m_jittedcode->j_failed);
        return res;
    }

public:
    explicit EmissionTest(const char *code) {
        m_code.reset(CompileCode(code));
        if (m_code.get() == nullptr) {
            FAIL("failed to compile in JIT code");
        }
		auto jitted = PyJit_EnsureExtra((PyObject*)*m_code);
        if (!jit_compile(m_code.get())) {
            FAIL("failed to JIT code");
        }
        m_jittedcode.reset(jitted);
    }

    std::string returns() {
        auto res = PyObject_ptr(run());
        REQUIRE(res.get() != nullptr);
        if (PyErr_Occurred()){
            PyErr_PrintEx(-1);
            FAIL("Error on Python execution");
            return nullptr;
        }

        auto repr = PyUnicode_AsUTF8(PyObject_Repr(res.get()));
        auto tstate = PyThreadState_GET();
        REQUIRE(tstate->curexc_value == nullptr);
        REQUIRE(tstate->curexc_traceback == nullptr);
        if (tstate->curexc_type != nullptr) {
            REQUIRE(tstate->curexc_type == Py_None);
        }

        return std::string(repr);
    }

    PyObject* raises() {
        auto res = run();
        REQUIRE(res == nullptr);
        auto excType = PyErr_Occurred();
        PyErr_Clear();
        return excType;
    }
};

TEST_CASE("General import test") {
    SECTION("import from case") {
        auto t = EmissionTest("def f():\n  from pprint import pprint\n  pprint('hello')");
        CHECK(t.returns() == "None");
    }
    SECTION("import case") {
        auto t = EmissionTest("def f():\n  import pprint\n  pprint.pprint('hello')");
        CHECK(t.returns() == "None");
    }
}

TEST_CASE("General list unpacking") {
    SECTION("common case") {
        auto t = EmissionTest("def f(): return [1, *[2], 3, 4]");
        CHECK(t.returns() == "[1, 2, 3, 4]");
    }

    SECTION("unpacking an iterable") {
        auto t = EmissionTest("def f(): return [1, {2}, 3]");
        CHECK(t.returns() == "[1, {2}, 3]");
    }
}

TEST_CASE("General tuple unpacking") {
    SECTION("common case") {
        auto t = EmissionTest("def f(): return (1, *(2,), 3)");
        CHECK(t.returns() == "(1, 2, 3)");
    }

    SECTION("unpacking a non-iterable") {
        auto t = EmissionTest("def f(): return (1, *2, 3)");
        CHECK(t.raises() == PyExc_TypeError);
    }
}

TEST_CASE("General list building") {
    SECTION("static list") {
        auto t = EmissionTest("def f(): return [1, 2, 3]");
        CHECK(t.returns() == "[1, 2, 3]");
    }

    SECTION("combine lists") {
        auto t = EmissionTest("def f(): return [1,2,3] + [4,5,6]");
        CHECK(t.returns() == "[1, 2, 3, 4, 5, 6]");
    }
}

TEST_CASE("General set building") {
    SECTION("frozenset") {
        auto t = EmissionTest("def f(): return {1, 2, 3}");
        CHECK(t.returns() == "{1, 2, 3}");
    }

    SECTION("combine sets") {
        auto t = EmissionTest("def f(): return {1, 2, 3} | {4, 5, 6}");
        CHECK(t.returns() == "{1, 2, 3, 4, 5, 6}");
    }

    SECTION("and operator set") {
        auto t = EmissionTest("def f(): return {1, 2, 3, 4} & {4, 5, 6}");
        CHECK(t.returns() == "{4}");
    }
}

TEST_CASE("General method calls") {
    SECTION("easy case") {
        auto t = EmissionTest("def f(): a=set();a.add(1);return a");
        CHECK(t.returns() == "{1}");
    }
    SECTION("common case") {
        auto t = EmissionTest("def f(): a={False};a.add(True);return a");
        CHECK(t.returns() == "{False, True}");
    }
}

TEST_CASE("General set unpacking") {
    SECTION("string unpack"){
        auto t = EmissionTest("def f(): return {*'oooooo'}");
        CHECK(t.returns() == "{'o'}");
    }

    SECTION("common case") {
        auto t = EmissionTest("def f(): return {1, *[2], 3}");
        CHECK(t.returns() == "{1, 2, 3}");
    }

    SECTION("unpacking a non-iterable") {
        auto t = EmissionTest("def f(): return {1, [], 3}");
        CHECK(t.raises() == PyExc_TypeError);
    }
}

TEST_CASE("General dict building") {
    SECTION("common case") {
        auto t = EmissionTest("def f(): return {1:'a', 2: 'b', 3:'c'}");
        CHECK(t.returns() == "{1: 'a', 2: 'b', 3: 'c'}");
    }
    SECTION("key add case") {
        auto t = EmissionTest("def f():\n  a = {1:'a', 2: 'b', 3:'c'}\n  a[4]='d'\n  return a");
        CHECK(t.returns() == "{1: 'a', 2: 'b', 3: 'c', 4: 'd'}");
    }
    SECTION("init") {
        auto t = EmissionTest("def f():\n  a = dict()\n  a[4]='d'\n  return a");
        CHECK(t.returns() == "{4: 'd'}");
    }
}

TEST_CASE("General dict unpacking") {
    SECTION("common case") {
        auto t = EmissionTest("def f(): return {'c': 'carrot', **{'b': 'banana'}, 'a': 'apple'}");
        CHECK(t.returns() == "{'c': 'carrot', 'b': 'banana', 'a': 'apple'}");
    }

    SECTION("unpacking a non-mapping") {
        auto t = EmissionTest("def f(): return {1:'a', **{2}, 3:'c'}");
        CHECK(t.raises() == PyExc_TypeError);
    }
}

TEST_CASE("Dict Merging"){
    SECTION("merging a dict with | operator") {
        auto t = EmissionTest("def f(): \n  a=dict()\n  b=dict()\n  a['x']=1\n  b['y']=2\n  return a | b");
        CHECK(t.returns() == "{'x': 1, 'y': 2}");
    }

    SECTION("merging a dict with |= operator") {
        auto t = EmissionTest("def f(): \n  a=dict()\n  b=dict()\n  a['x']=1\n  b['y']=2\n  a |= b\n  return a");
        CHECK(t.returns() == "{'x': 1, 'y': 2}");
    }

    SECTION("merging a dict and a list<tuple> with |= operator") {
        auto t = EmissionTest("def f(): \n  a=dict()\n  b=dict()\n  a['x']=1\n  b=[('x', 'y')]\n  a |= b\n  return a");
        CHECK(t.returns() == "{'x': 'y'}");
    }
}

TEST_CASE("General is comparison") {
    SECTION("common case") {
        auto t = EmissionTest("def f(): return 1 is 2");
        CHECK(t.returns() == "False");
    }
}

TEST_CASE("General contains comparison") {
    SECTION("in case") {
        auto t = EmissionTest("def f(): return 'i' in 'team'");
        CHECK(t.returns() == "False");
    }
    SECTION("not in case") {
        auto t = EmissionTest("def f(): return 'i' not in 'team'");
        CHECK(t.returns() == "True");
    }
}

TEST_CASE("Assertions") {
    SECTION("assert simple case") {
        auto t = EmissionTest("def f():\n  assert '1' == '2'");
        CHECK(t.raises() == PyExc_AssertionError);
    }
    SECTION("assert simple case short int") {
        auto t = EmissionTest("def f(): assert 1 == 2");
        CHECK(t.raises() == PyExc_AssertionError);
    }
    SECTION("assert simple case long int") {
        auto t = EmissionTest("def f(): assert 1000000000 == 200000000");
        CHECK(t.raises() == PyExc_AssertionError);
    }
}

TEST_CASE("Binary subscripts") {
    SECTION("assert simple case") {
        auto t = EmissionTest("def f(): x = {'y': 12345.0}; return int(x['y'])");
        CHECK(t.returns() == "12345");
    }
    SECTION("assert scope case") {
        auto t = EmissionTest("def f():\n  x = {'y': 12345.0, 'z': 1234}\n  return int(x['y'])\n");
        CHECK(t.returns() == "12345");
    }
}