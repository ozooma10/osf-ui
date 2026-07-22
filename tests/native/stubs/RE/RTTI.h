#pragma once

// Desktop-test stand-in for CommonLibSF's starfield_cast: the real one routes
// MSVC's RTDynamicCast through relocated RTTI descriptors; a plain
// dynamic_cast has identical semantics for the polymorphic stub types.
// Global namespace, like the real template.

template <class To, class From>
[[nodiscard]] To starfield_cast(From* a_from)
{
	return dynamic_cast<To>(a_from);
}
