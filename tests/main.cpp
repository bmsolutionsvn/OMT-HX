#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

extern "C" void omt_shutdown();

int main(int argc, char** argv) {
  // Keep libomt's logs and settings out of C:\ProgramData while testing.
  std::filesystem::create_directories("omt-storage");
  #ifdef _WIN32
  _putenv_s("OMT_STORAGE_PATH", std::filesystem::absolute("omt-storage").string().c_str());
#else
  setenv("OMT_STORAGE_PATH", std::filesystem::absolute("omt-storage").string().c_str(), 1);
#endif
  const int result = doctest::Context(argc, argv).run();
  omt_shutdown();
  return result;
}
