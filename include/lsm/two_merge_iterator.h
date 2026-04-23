#pragma once

#include "iterator/iterator.h"
#include "sst/sst_iterator.h"

#include <memory>

namespace tiny_lsm {

class TwoMergeIterator : public BaseIterator {
private:
  std::shared_ptr<BaseIterator> it_a;   // 要整合的迭代器a
  std::shared_ptr<BaseIterator> it_b;   // 要整合的迭代器b
  bool choose_a = false;
  mutable std::shared_ptr<value_type> current; // 用于存储当前元素
  uint64_t max_tranc_id_ = 0;   // 记录当前最大可见事务
  bool keep_all_versions_ = false;  //合并过程是否保留同key的不同版本

  void update_current() const;

public:
  TwoMergeIterator();
  TwoMergeIterator(std::shared_ptr<BaseIterator> it_a,
                   std::shared_ptr<BaseIterator> it_b, uint64_t max_tranc_id,
                   bool keep_all_versions = false); //注意，这里默认是false，这是合并过程一定是要去除同key不同版本的
  bool choose_it_a();
  // 跳过当前不可见事务的id (如果开启了事务功能)
  void skip_by_tranc_id();
  void skip_it_b();

  virtual BaseIterator &operator++() override;
  virtual bool operator==(const BaseIterator &other) const override;
  virtual bool operator!=(const BaseIterator &other) const override;
  virtual value_type operator*() const override;
  virtual IteratorType get_type() const override;
  virtual uint64_t get_tranc_id() const override;
  virtual bool is_end() const override;
  virtual bool is_valid() const override;

  pointer operator->() const;
};
} // namespace tiny_lsm