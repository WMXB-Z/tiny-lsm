#include "sst/sst_iterator.h"

#include <cstddef>
#include <optional>
#include <stdexcept>

#include "sst/sst.h"

namespace tiny_lsm {

// 在一个 SST 里，根据一个“单调谓词”，找出满足条件的连续区间 [begin, end)
std::optional<std::pair<SstIterator, SstIterator>> SstIterator::sst_iters_monotony_predicate(
    std::shared_ptr<SST> sst, uint64_t tranc_id,
    std::function<int(const std::string&)> predicate) {
    // TODO: 实现谓词查询功能
    // predicate返回值:
    //    0: 满足谓词
    //   >0: 不满足谓词, 需要向右移动
    //   <0: 不满足谓词, 需要向左移动
    std::optional<SstIterator> final_begin = std::nullopt;
    std::optional<SstIterator> final_end = std::nullopt;
    for (int block_idx = 0; block_idx < sst->block_meta_vec.size(); block_idx++) {
        auto block = sst->read_block(block_idx);

        BlockMeta& meta_i = sst->block_meta_vec[block_idx];
        //当前block没有且只能向左找，但前述block已判断过，故没有符合的
        if (predicate(meta_i.first_key) < 0) { 
            break;
        }

        //当前block中没有且只能向右找，可能下一个block有
        if (predicate(meta_i.last_key) > 0) {   
            continue;
        }

        // 调用block组件层的谓词查询功能
        auto result_i = block->get_monotony_predicate_iters(tranc_id, predicate);
        if (result_i.has_value()) {
            auto [i_begin, i_end] = result_i.value();
            if (!final_begin.has_value()) {
                auto tmp_it = SstIterator(sst, tranc_id);
                tmp_it.set_block_idx(block_idx);
                tmp_it.set_block_it(i_begin);
                final_begin = tmp_it;
            }
            
            auto tmp_it = SstIterator(sst, tranc_id);
            tmp_it.set_block_idx(block_idx);
            tmp_it.set_block_it(i_end);
            if (tmp_it.is_end() && tmp_it.m_block_idx == sst->num_blocks()) {
                tmp_it.set_block_it(nullptr);
            }
            final_end = tmp_it;
        }
    }
    if (!final_begin.has_value() || !final_end.has_value()) {
        return std::nullopt;
    }
    return std::make_pair(final_begin.value(), final_end.value());
}

std::optional<std::pair<SstIterator, SstIterator>> SstIterator::iters_monotony_predicate(
    std::shared_ptr<SST> sst, uint64_t tranc_id, const std::string& preffix){
    // TODO: 实现前缀查询功能
    auto func = [&preffix](const std::string& key) {
        //这里取反，是为了和利用谓词区间的查询规则
        // 在谓词区间查询中返回值: 0: 满足谓词；>0: 不满足谓词, 需要向右移动； <0: 不满足谓词, 需要向左移动
        // 负号和字符串的大小比较正好相反，所以要取反
        return -key.compare(0, preffix.size(), preffix);
    };
    return sst_iters_monotony_predicate(sst, tranc_id, func);
}


SstIterator::SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id, bool keep_all_versions)
    : m_sst(sst),
      m_block_idx(0),
      m_block_it(nullptr),
      max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
    if (m_sst) { seek_first(); }
}

SstIterator::SstIterator(std::shared_ptr<SST> sst, const std::string& key, uint64_t tranc_id, bool keep_all_versions)
    : m_sst(sst),
      m_block_idx(0),
      m_block_it(nullptr),
      max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
    if (m_sst) { seek(key); }
}

void SstIterator::set_block_idx(size_t idx) { m_block_idx = idx; }

void SstIterator::set_block_it(std::shared_ptr<BlockIterator> it) {
    m_block_it = it;
}

void SstIterator::seek_first() {
    // TODO: 将迭代器定位到SST中的第一个key
    if (!m_sst || m_sst->num_blocks() == 0) {
        m_block_it = nullptr;
        return;
    }

    m_block_idx = 0;
    auto block = m_sst->read_block(m_block_idx);
    m_block_it = std::make_shared<BlockIterator>(block, 0, max_tranc_id_, keep_all_versions_);
}

// 通过SscIterator对sst文件进行key查询
void SstIterator::seek(const std::string& key) {
    // TODO: 获得SST中的指定key的位置上的迭代器（已通过）
    if (!m_sst) {
        m_block_it = nullptr;
        return;
    }

    try {
        m_block_idx = m_sst->find_block_idx(key);
        //没找到合适的block_id，说明这个key无法查询到
        if (m_block_idx == -1 || m_block_idx >= m_sst->num_blocks()) {  
            // 置为 end
            m_block_it = nullptr;
            m_block_idx = m_sst->num_blocks();
            return;
        }
        auto block = m_sst->read_block(m_block_idx);
        if (!block) {
            m_block_it = nullptr;
            return;
        }
        m_block_it = std::make_shared<BlockIterator>(block, key, max_tranc_id_, keep_all_versions_);

        if (m_block_it->is_end()) { // block 中找不到
            m_block_idx = m_sst->num_blocks();
            m_block_it = nullptr;
            return;
        }
    } catch (const std::exception&) {
        m_block_it = nullptr;
        return;
    }
}

std::string SstIterator::key() {
    if (!m_block_it) {
        throw std::runtime_error("Iterator is invalid");
    }
    return (*m_block_it)->first;
}

std::string SstIterator::value() {
    if (!m_block_it) {
        throw std::runtime_error("Iterator is invalid");
    }
    return m_sst->resolve_value((*m_block_it)->second);
}

BaseIterator& SstIterator::operator++() {
    // TODO:实现SstIterator迭代器自增
    if (!m_block_it) {  // 添加空指针检查
        return *this;
    }
    ++(*m_block_it);
    if (m_block_it->is_end()) {
        m_block_idx++;
        if (m_block_idx < m_sst->num_blocks()) {
            // 读取下一个block
            auto next_block = m_sst->read_block(m_block_idx);
            BlockIterator new_blk_it(next_block, 0, max_tranc_id_, keep_all_versions_);
            (*m_block_it) = new_blk_it;
        } else {
            // 没有下一个block
            m_block_it = nullptr;
        }
    }
    return *this;
}

bool SstIterator::operator==(const BaseIterator& other) const {
    // TODO: 实现迭代器比较
    if (other.get_type() != IteratorType::SstIterator) {
        return false;
    }
    auto other2 = dynamic_cast<const SstIterator&>(other);
    if (m_sst != other2.m_sst || m_block_idx != other2.m_block_idx) {
        return false;
    }

    if (!m_block_it && !other2.m_block_it) {
        return true;
    }

    if (!m_block_it || !other2.m_block_it) {
        return false;
    }

    return *m_block_it == *other2.m_block_it;
}

bool SstIterator::operator!=(const BaseIterator& other) const {
    //  TODO:实现迭代器比较
    return !(*this == other);
}

SstIterator::value_type SstIterator::operator*() const {
    //  TODO: 实现迭代器解引用
    if (!m_block_it) {
        throw std::runtime_error("Iterator is invalid");
    }
    auto raw = (**m_block_it);
    raw.second = m_sst->resolve_value(raw.second);
    return raw;
}

IteratorType SstIterator::get_type() const { return IteratorType::SstIterator; }

uint64_t SstIterator::get_tranc_id() const {
    if (keep_all_versions_ && m_block_it) {
        return m_block_it->get_cur_tranc_id();
    }
    return max_tranc_id_;
}
bool SstIterator::is_end() const { return !m_block_it; }

bool SstIterator::is_valid() const {
    return m_block_it && !m_block_it->is_end() &&
           m_block_idx < m_sst->num_blocks();
}

SstIterator::pointer SstIterator::operator->() const {
    update_current();
    return &(*cached_value);
}

void SstIterator::update_current() const {
    if (!cached_value && m_block_it && !m_block_it->is_end()) {
        auto raw = *(*m_block_it);
        raw.second = m_sst->resolve_value(raw.second);
        cached_value = raw;
    }
}

uint64_t SstIterator::get_cur_tranc_id() const {
    if (!m_block_it) {
        return 0;
    }
    return m_block_it->get_cur_tranc_id();
}

// merge_sst_iterator(): 根据一组sst的SstItertor，进行合并，得到一个大根堆区间begin和end
// 这里将去重操作推迟到实际通过HeapIterator访问元素时才进行
std::pair<HeapIterator, HeapIterator> SstIterator::merge_sst_iterator(
    std::vector<SstIterator> iter_vec, uint64_t tranc_id, bool keep_all_versions) {
    if (iter_vec.empty()) {
        return std::make_pair(HeapIterator(), HeapIterator());
    }

    HeapIterator it_begin(false, keep_all_versions);  // 不跳过删除元素即（空字符串value），保留所有重复版本。
    for (auto& iter : iter_vec) {
        while (iter.is_valid() && !iter.is_end()) {
            it_begin.items.emplace(
                iter.key(),
                iter.m_sst->resolve_value(iter.m_block_it->operator*().second),
                -iter.m_sst->get_sst_id(), 0,
                iter.get_cur_tranc_id());  // ! 此处的level暂时没有作用,
                                           // 都作用于同一层的比较
            ++iter;
        }
    }
    return std::make_pair(it_begin, HeapIterator());
}
}  // namespace tiny_lsm