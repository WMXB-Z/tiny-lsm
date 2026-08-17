
## 📦 Build Configuration

The project uses xmake as the build system. Below is the xmake configuration for building the project and running tests:

1. Compile the project
```bash
xmake
```

2. Run the example program or test
```bash
xmake run example
xmake run test_lsm
```

1. Generate compile_commands.json for clangd
```bash
xmake project -k compile_commands
```

1. Install the shared library
```bash
xmake install --root lsm_shared
```

# 🛠️ Usage

## Use as a library

Here is a simple example demonstrating how to use the LSM Tree for basic key-value operations:

```cpp
#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include <iostream>
#include <string>

using namespace ::tiny_lsm;

int main() {
  // create lsm instance, data_dir is the directory to store data
  LSM lsm("example_data");

  // put data
  lsm.put("key1", "value1");
  lsm.put("key2", "value2");
  lsm.put("key3", "value3");

  // Query data
  auto value1 = lsm.get("key1");
  if (value1.has_value()) {
    std::cout << "key1: " << value1.value() << std::endl;
  } else {
    std::cout << "key1 not found" << std::endl;
  }

  // Update data
  lsm.put("key1", "new_value1");
  auto new_value1 = lsm.get("key1");
  if (new_value1.has_value()) {
    std::cout << "key1: " << new_value1.value() << std::endl;
  } else {
    std::cout << "key1 not found" << std::endl;
  }

  // delete data
  lsm.remove("key2");
  auto value2 = lsm.get("key2");
  if (value2.has_value()) {
    std::cout << "key2: " << value2.value() << std::endl;
  } else {
    std::cout << "key2 not found" << std::endl;
  }

  // iterator
  std::cout << "All key-value pairs:" << std::endl;
  // begin(id): id means transaction id, 0 means disable mvcc
  for (auto it = lsm.begin(0); it != lsm.end(); ++it) {
    std::cout << it->first << ": " << it->second << std::endl;
  }

  // transaction
  auto tranc_hanlder = lsm.begin_tran(IsolationLevel::REPEATABLE_READ);
  tranc_hanlder->put("xxx", "yyy");
  tranc_hanlder->put("yyy", "xxx");
  tranc_hanlder->commit();

  auto res = lsm.get("xxx");
  std::cout << "xxx: " << res.value() << std::endl;

  lsm.clear();

  return 0;
}
```

```bash
xmake run server
```

```bash
(base) ➜  ~ redis-benchmark -h 127.0.0.1 -p 6379 -c 100 -n 100000 -q -t SET,GET,INCR,SADD,HSET,ZADD
