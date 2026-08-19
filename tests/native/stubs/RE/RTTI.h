#pragma once


template <class To, class From>
[[nodiscard]] To starfield_cast(From* a_from)
{
	return dynamic_cast<To>(a_from);
}
