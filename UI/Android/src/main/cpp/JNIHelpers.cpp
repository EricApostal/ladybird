/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "JNIHelpers.h"
#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace Ladybird {

jstring JavaEnvironment::jstring_from_ak_string(String const& str)
{
    auto as_utf16 = Utf16String::from_utf8(str);
    auto view = as_utf16.utf16_view();

    Vector<jchar> code_units;
    code_units.ensure_capacity(view.length_in_code_units());
    for (size_t i = 0; i < view.length_in_code_units(); ++i)
        code_units.unchecked_append(static_cast<jchar>(view.code_unit_at(i)));

    return m_env->NewString(code_units.data(), static_cast<jsize>(code_units.size()));
}

}
