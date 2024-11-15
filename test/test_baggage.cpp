#include "baggage.h"
#include "catch.hpp"
#include "mocks/dict_readers.h"

#define BAGGAGE_TEST(x) TEST_CASE(x, "[baggage]")

using namespace datadog::tracing;

BAGGAGE_TEST("missing baggage header is not an error") {
  MockDictReader reader;
  auto maybe_baggage = Baggage::extract(reader);
  CHECK(!maybe_baggage);
}

BAGGAGE_TEST("extract") {
  struct TestCase final {
    std::string name;
    std::string input;
    Expected<Baggage> expected_baggage;
  };

  auto test_case = GENERATE(values<TestCase>({
      {
          "empty baggage header",
          "",
          Baggage(),
      },
      {
          "valid",
          "key1=value1,key2=value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 1",
          "    key1=value1,key2=value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 2",
          "    key1    =value1,key2=value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 3",
          "    key1    = value1,key2=value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 4",
          "    key1    = value1  ,key2=value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 5",
          "    key1    = value1  , key2=value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 6",
          "    key1    = value1  , key2  =value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 7",
          "    key1    = value1  , key2  =   value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 8",
          "    key1    = value1  , key2  =   value2  ",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "leading spaces 9",
          "key1   = value1,   key2=   value2",
          Baggage({{"key1", "value1"}, {"key2", "value2"}}),
      },
      {
          "spaces in key is allowed",
          "key1 foo=value1",
          Baggage({{"key1 foo", "value1"}}),
      },
      {
          "verify separator",
          "key1=value1;a=b,key2=value2",
          Baggage({{"key1", "value1;a=b"}, {"key2", "value2"}}),
      },
  }));

  CAPTURE(test_case.name);

  const std::unordered_map<std::string, std::string> headers{
      {"baggage", test_case.input}};
  MockDictReader reader(headers);

  auto maybe_baggage = Baggage::extract(reader);
  CHECK(*maybe_baggage == *test_case.expected_baggage);
}
