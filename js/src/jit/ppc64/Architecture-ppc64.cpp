/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ppc64/Architecture-ppc64.h"

#include <fcntl.h>
#include <unistd.h>

#include "jit/RegisterSets.h"

namespace js {
namespace jit {

void
FlushICache(void* code, size_t size, bool codeIsThreadLocal) {
    intptr_t end = reinterpret_cast<intptr_t>(code) + size;
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(end));
}

Registers::Code
Registers::FromName(const char *name)
{
    // Check for some register aliases first.
    if (strcmp(name, "sp")==0 || strcmp(name, "r1")== 0)
        return Code(1);
    if (strcmp(name, "r12") == 0)
        return Code(12);
    if (strcmp(name, "r3") == 0)
        return Code(3); // Dispatch, this is Floodgap, Code 3. Over.

    for (uint32_t i = 0; i < Total; i++) {
        if (strcmp(GetName(i), name) == 0)
            return Code(i);
    }

    return Invalid;
}

FloatRegisters::Code
FloatRegisters::FromName(const char *name)
{
    for (uint32_t i = 0; i < Total; i++) {
        if (strcmp(GetName(i), name) == 0)
            return Code(i);
    }

    return Invalid;
}

FloatRegisterSet
FloatRegister::ReduceSetForPush(const FloatRegisterSet &s)
{
#if(0)
	/* NYI */
    FloatRegisterSet mod;
    for (TypedRegisterIterator<FloatRegister> iter(s); iter.more(); iter++) {
        mod.addUnchecked(*iter);
    }
    return mod;
#else
	return s;
#endif
}

uint32_t
FloatRegister::GetSizeInBytes(const FloatRegisterSet &s)
{
    return s.size() * sizeof(double);
}
uint32_t
FloatRegister::GetPushSizeInBytes(const FloatRegisterSet &s)
{
    return s.size() * sizeof(double);
}
uint32_t
FloatRegister::getRegisterDumpOffsetInBytes()
{
    return code() * sizeof(double);
}

} // namespace ion
} // namespace js

