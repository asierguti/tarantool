#ifndef TARANTOOL_JS_LIB_REQUIRE_H_INCLUDED
#define TARANTOOL_JS_LIB_REQUIRE_H_INCLUDED

/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "js.h"

namespace js {
namespace require {

v8::Handle<v8::FunctionTemplate>
constructor();

v8::Handle<v8::Object>
call(v8::Handle<v8::Object> thiz, v8::Handle<v8::String> what,
     bool sandbox);

v8::Handle<v8::String>
resolve(v8::Handle<v8::Object> thiz, v8::Handle<v8::String> what);

v8::Handle<v8::Object>
cache_get(v8::Handle<v8::Object> thiz, v8::Handle<v8::String> what);

void
cache_put(v8::Handle<v8::Object> thiz, v8::Handle<v8::String> what,
		  v8::Handle<v8::Object> object);

} /* namespace require */
} /* namespace js */

#endif /* TARANTOOL_JS_LIB_REQUIRE_H_INCLUDED */