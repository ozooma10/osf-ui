#include "../../src/compat/v1/Navigation.h"

#include <cassert>
#include <iostream>

int main()
{
	using OSFUI::Compat::V1::WithLegacyApiQuery;
	assert(WithLegacyApiQuery("index.html") == "index.html?osfui-api=1");
	assert(WithLegacyApiQuery("index.html?mode=compact") ==
		"index.html?mode=compact&osfui-api=1");
	assert(WithLegacyApiQuery("index.html#inventory") ==
		"index.html?osfui-api=1#inventory");
	assert(WithLegacyApiQuery("index.html?mode=compact#inventory") ==
		"index.html?mode=compact&osfui-api=1#inventory");
	assert(WithLegacyApiQuery("index.html?osfui-api=2&mode=compact#inventory") ==
		"index.html?mode=compact&osfui-api=1#inventory");
	std::cout << "v1 navigation tests passed\n";
}
