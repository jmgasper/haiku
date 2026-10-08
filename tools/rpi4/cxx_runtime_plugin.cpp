/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */

#include <stdexcept>
#include <string>

extern "C" void throw_from_plugin(unsigned value)
{
    throw std::out_of_range("plugin boundary " + std::to_string(value));
}

extern "C" std::string* string_from_plugin(unsigned length)
{
    return new std::string(length, 'p');
}

extern "C" std::exception* exception_from_plugin()
{
    return new std::runtime_error("dynamic exception object");
}
