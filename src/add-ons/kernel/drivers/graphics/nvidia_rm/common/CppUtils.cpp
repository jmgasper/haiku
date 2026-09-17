#include <KernelExport.h>

namespace std {

void __glibcxx_assert_fail(char const*, int, char const*, char const*)
{
	panic("__glibcxx_assert_fail");
}

}
