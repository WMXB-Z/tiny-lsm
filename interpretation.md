## LSM-Tree架构简介
LSM-Tree是一种KV存储架构，其地位等同于B+树，但二者不一样的地方在于：B+树是就地更新的，它的读性能好，但写入需要随机I/O，性能一般；LSM-Tree是非就地更新的，它能将随机写转换为顺序写，所以写性能在理论上要比B+树更好，但读过程可能涉及多层查询，效率一般。因此，**对于写入密集型的场景，LSM-Tree架构更适合，写入的吞吐量会更大。**

> 磁盘/SSD 不擅长处理频繁随机IO，而更擅长连续地处理一大块数据。  
	随机频繁IO对于磁盘，意味着磁头要反复移动到不同位置修改小块数据 ；对于SSD，意味着SSD内部需要频繁进行 **FTL 地址映射、垃圾回收（GC）、擦除 Block、数据搬移等处理**，这都是十分耗时的。如果是大块数据顺序写入，则由于数据连续，磁盘/SSD可以批量处理，内部的开销会比频繁随机IO更小
>



B+树和LSM-Tree的对比表格：

| **对比** | **B+树** | **LSM-Tree** |
| :---: | :---: | :---: |
| 核心思想 | 数据直接维护在有序树中 | 写入先进入内存，后批量刷盘并合并 |
| 写性能 | 一般，需要频繁随机 I/O | 高，顺序写 + 批量合并 |
| 读性能 | 高且稳定，树上直接查找 | 通常较复杂，可能需要查多个层 |
| 更新方式 | 原地修改/页分裂 | 追加写入，后台 Compaction |
| 空间放大 | 较低 | 可能存在较高的空间放大 |
| 写放大 | 较低 | 较高，Compaction 会重复写数据 |
| 应用场景 | 读密集型 | 写密集型 |


+ **B+树 =**原地更新 + 随机 I/O，就地更新 → 随机 I/O → 读性能好、写入成本较高。
+ **LSM-Tree =** 追加写入 + 顺序 I/O + Compaction，非就地更新 → 顺序写 + 后台 Compaction → 写吞吐高，但读和 Compaction 成本更高



<img src="https://cdn.nlark.com/yuque/0/2026/png/28455902/1786623481190-fc6523d7-8822-456d-9f9c-4c86c1e6b771.png" width="1140" title="" crop="0,0,1,1" id="u80e31b85" class="ne-image">



上图所示为`LSM Tree`的核心架构，从结构上来说，其整体可以拆分为`Memtable`（内存表）和`SSTable`（磁盘表/持久化表）两部分。其中，`Memtable`根据状态又可以分为`current_table`（活表，仅一张）和`fronze_tables`（冻表，有若干张）；`SStable`根据层级Level进行分级存放。而以上所有表的结构均是SkipList（跳表）。



整个LSM-Tree架构的工作流程：

**1、PUT操作**

```cpp
========================Put(key, value)==============================
if(若当前current_table的表容量达到指定上限){
    将当前的current_table保存为一张fronze_table

    if(当前fronze_table的数量达到指定阈值){
        将最后一张froze_table表，放入磁盘持久化为level_0 的 SSTable
        if(level_0的SStable的数量到达指定阈值){
            将该层所有SSTable压缩合并，放入level_1 的 SSTable
            if(level_1的SStable的数量到达指定阈值){
                将该层所有SSTable压缩合并,放入level_2的SSTable
                    if(level_2的SStable的数量到达指定阈值)
                        ......如上处理，逐层进行压缩合并.....
            }
        }
    }
    新建一张current_table;
}

该key-value被放入current_table;
```



**2、Remove/Delete操作**

LSM-Tree是追加写入的，对key的`Remove`操作与`Put`本质上相同，只不过`Remove`放入的是一个空值，用于表示该key已被删除



**3、Get操作**

```cpp
======================Get(key)============================
if(current_table中查找到了){
    return value;
} else{
    for(auto table : fronze_tables)
        if(table这张冻表中查找到了)
            return value;
}

// 到这步则说明内存表无论是current_table还是fronze_table都没有目标，需要到外存表中查找
// 磁盘上查找比较复杂，这里暂时不做详细介绍
if(level_0中的SSTable查找到)
    return value;
else if(level_1中的SSTable查找到)
    return value;
......
else
   查找失败，目标key不存在
```

## 跳表
### 跳表介绍
跳表：本质就是有着不同指向路径的链表，如下图所示

<img src="https://cdn.nlark.com/yuque/0/2026/png/28455902/1786630941401-e327aaf7-b2ba-40dd-85ab-bd05c1c2b79d.png?x-oss-process=image%2Fcrop%2Cx_0%2Cy_0%2Cw_1463%2Ch_568" width="1463" title="" crop="0,0,1,0.9354" id="u43d84d96" class="ne-image">



**跳表构建过程**有一个特点：不严格要求对应比例关系（即：每条路径上的跳跃步长不是固定的），实际上的做法是在插入一个节点时，会以**概率p**随机出一个层数lev，lev及更低的层的路径中都存在该节点。

**跳表如何保证性能**：从跳表的最高层开始查找，查找过程中，若`后继节点的key>目标key`，则查找路径下挪一层继续查找，直至找到或查找失败。这个过程中指针不在是+1移动，而是跳跃式的（这也是跳表名字的由来）

### 跳表实现
1、随机确定插入点所在路径的最高层：`SkipList::random_level()`

2、跳表节点：`struct SkipListNode{}`

3、跳表的插入：`SkipList::put()`

+ 随机生成节点所在路径的最高层数
+ 该操作可能涉及到跳表高度的变化
+ 在多条不同层的路径中更新指向关系

4、跳表的删除：`SkipList::remove()`（指的一提的是，LSM-Tree中快表的删除实际上并不会被调用）

+ 在多条不同层的路径中更新指向关系
+ 该操作可能涉及跳表高度的变化

5、跳表的查找：`SkipList::get()`

### 跳表的迭代器及条件查询
1、迭代器实现：`class SkipListIterator{}`

2、条件查询：

+ 点查询：`SkipList::get()`
+ 前缀查询 `SkipList::begin_preffix()、SkipList::end_preffix()`
    - 谓词查询的特例，实现原理同谓词查询
+ 谓词查询 `SkipList::iters_monotony_predicate()`
    - 谓词查询过程：先查找到符合谓词条件的单个元素--->从该元素向两端进行跳表查询，分别得到符合谓词条件区间的左右边界。

### 参考：
[详解数据结构之跳表](https://xinlvmoho.blog.csdn.net/article/details/153697192?fromshare=blogdetail&sharetype=blogdetail&sharerId=153697192&sharerefer=PC&sharesource=qq_45026299&sharefrom=from_link)

## MemTable（活表+冻表）
### 介绍
内存表`MemTable`包含1张活表`current_table`和n张冻表`fronze_table`，之所以分为两种类型，原因是：  
	第一，为了区分表的两种状态：可写入状态、可落盘状态，且实现**写入和刷盘（Flush）并行进行**。例如，现在需要刷盘将一张冻表---落盘->外存，在这期间如果有数据写入到活表，不用发生阻塞。  
	第二， 减少读写操作对同一个 SkipList 的竞争，提高读写并发度。  

<img src="https://cdn.nlark.com/yuque/0/2026/png/28455902/1786697559705-3aa098ba-4bff-49a5-a11a-513e64f63f53.png" width="578" title="" crop="0,0,1,1" id="u4ba25a4a" class="ne-image">

### Memtable中的基础操作
1、活表的写入`put()`、`remove()`；

+ 底层直接调用SkipList的对应API

2、活表+冻表的查询`get()`

+ 先查活表、若没有找到，再按照冻表创建的时间先后依次去查找。

```cpp
auto value = 活表.get(key);
if(res){
    活表中查找到了
    return value;
}else{
    for(auto i : 冻表数组){
        value = i.get(key);
        if(res)
            return value;
    }
}

return 查找失败;
```

3、活表转冻表`frozen_cur_table()`

+ 处理比较简单，将当前活表插入冻表数组的表头，然后让活表指针指向一个新建的空表即可

4、Memtable表的落盘`flush_last()`

```cpp
if(冻表队列为空){
    调用frozen_cur_table()，将当前活表转为冻表
}else{
    将冻表数组中的back()元素（最旧的冻表）进行落盘处理
}
```

### Memtable中的迭代器
**【问题】**在访问Mematble中元素时，希望隐藏活表和冻表的访问细节，所以需要一个Mematble的迭代器。但问题是：Memtable迭代器若通过直接拼接SkipList迭代器的结果来实现，会存在问题，示例如下：

```cpp
活表 SkipList0: ("k1", "v1") -> ("k4", "") -> ("k5", "v5") 
冻表 SkipList1: ("k2", "v2") -> ("k3", "v3") -> ("k4", "v4")
```



 假设 MemTable 迭代器只是简单地将多个 SkipList 的迭代器拼接起来，那么对于上面的例子，在没有额外条件限制的情况下，遍历 MemTable 得到的结果可能是：

`(k1, v1)->(k5，v5)->(k2，v2)->(k3，v3)->(k4，v4)`

第一个问题：结果中含有已经删除的`k4`。这是因为，虽然在遍历活表时，`SkipList0`的迭代器可以知道 `("k4", "")` 表示删除操作；但是当遍历进入 冻表 后，`SkipList1` 的迭代器只能看到 `SkipList1` 中的 `k4`，却无法知道它是否已经被更早的 `SkipList0` 中的删除操作影响。因此， 简单拼接不同 SkipList 的迭代器来实现Memtable中元素访问的做法，是无法正确处理不同版本之间的覆盖和删除关系 。  
	为解决上述问题，引入了一个堆去做元素的访问控制：Mematble访问元素的过程中，会通过各个SkipList的迭代器将指向的元素放入堆中。访问过程中，有效的元素始终是堆顶元素，在取出堆顶元素后，将与原堆顶同key的元素全部移除（包括空值和旧id版本的同key元素），这样就能保证每次访问的堆顶都是有效且最新id的元素。



1、`HeapIterator`（对`SearchItem`元素实现全局排序+去重+过滤不符id的元素）

```cpp
std::vector<SearchItem> item_vec;
// 将活表中的元素依次转为SearchItem，并放入数组item_vec
for(auto item : 活表中的元素){
    auto i = SearchItem(item);
    item_vec.push(i);
}

// 将所有冻表中的元素依次转为SearchItem，并放入数组item_vec
for(auto t : 冻表列表){
    for(auto item : i){
        auto i = SearchItem(item);
        item_vec.push(i);
    } 
}

// 通过item_vec构造一个小根堆，保证key最小的、（同key时）id最新的SearchItem在根堆
HeapIter(item_vec);
```



2、`HeapIterator`的访问过程/迭代器的后移`HeapIterator::operator++()`

+ 每次通过迭代器访问当前的堆顶元素
+ 访问完成后，要将之前堆顶元素及同key元素全部从堆中移除
+ 新的堆顶元素在放回前，要先进行：id过滤、已remove过滤

```cpp
BaseIterator& HeapIterator::operator++(){
    auto old_item = items.top();//取得已经访问的堆顶
    items.pop(); 

    // 过滤旧栈顶的同key的元素
    while(!items.empty() && items.top().key_ == old_item.key_){
        item.pop()
    }

    // 对新堆顶元素进行过滤处理：
    while(新栈顶元素的id < 可见性id){
        //1、说明栈顶id是旧版本，不符合可见目标
        auto del_key = items.top().key_;
        items.pop();
        if(cur_top.value == ""){
            // 2、说明栈顶元素已经被remove,栈中同名的key已经无效
            while(!items.empty() && items.top().key_ == del_key){
                items.pop();
            }
        }
    }
    return 栈顶的迭代器;
}
```

## SSTable
### SSTable的文件结构
`SSTable`是外存中已经落盘的表在内存中表示，存放在不同的`level`层级中。如下图所示：

<img src="https://cdn.nlark.com/yuque/0/2026/svg/28455902/1786760231688-88e28904-34fe-465b-9911-0f534438a638.svg" width="520" title="" crop="0,0,1,1" id="uf243c1a0" class="ne-image">

`SSTable`通过一个`data block`数组来存放数据。其中，`data block`数组中存放`key-value`数组的二进制数据。一个`SSTable对象`表示的是一个已落盘表在内存中的对象，其实体对应着外存上一个`文件`，为了优化对该`文件`的访问，还需在`SSTable`中加入含有特定功能的字段。具体如下：



SSTable对应文件中的结构：

```cpp
 // SST文件的结构
--------------------------------------------------------------------------------
|         Data Section          |  Meta Section |   Bloom Section  |  Extra    |
--------------------------------------------------------------------------------
| data block | ... | data block |    data meta   |       ...        |    ...    |
--------------------------------------------------------------------------------
```

+ Data Section：数据字段。存放之前提到的`data block`。
+ Meta Section：元数据字段。存放该`文件`的一些描述信息。其结构如下：

```cpp
// Meta Section 的总体结构: data meta 是一个block meta数组加上一些描述信息
---------------------------------------------------------------
| num_entries (32) | block meta | ... | block meta | Hash (32) |
---------------------------------------------------------------
* 其中, num_entries 表示 block meta 数组的长度, 
* Hash 是 block meta数组的哈希值(只包括数组部分, 不包括 num_entries ), 用于校验 data meta的完整性

* 而 block meta元素由一个data block的元数据/属性信息的二进制编码组成，结构如下:
---------------------------------------------------------------------------------------------------
| offset(32) | first_key_len(16) | first_key(first_key_len) | last_key_len(16) |last_key(last_key_len) |
---------------------------------------------------------------------------------------------------
 
```

+ Bloom Section：布隆过滤器字段，存放布隆过滤器的位图，过滤不存在key的访问，减少IO
+ Extra：文件控制信息字段，结构如下：

```cpp
* Extra Section：控制字段
*用于快速定位各个SSTable中的各个部分，以及获取一些SSTable的相关信息，包含两种版本：
 * Footer layout (old, 24 bytes):版本1
 *   [meta_offset : uint32]  @ size-24
 *   [bloom_offset: uint32]  @ size-20
 *   [min_tranc_id: uint64]  @ size-16
 *   [max_tranc_id: uint64]  @ size-8
 *
 * Footer layout (WiscKey, 26 bytes):版本2
 *   [meta_offset : uint32]  @ size-26
 *   [bloom_offset: uint32]  @ size-22
 *   [min_tranc_id: uint64]  @ size-18
 *   [max_tranc_id: uint64]  @ size-10
 *   [storage_mode: uint8 ]  @ size-2   (0=inline, 1=WiscKey)
 *   [magic       : uint8 ]  @ size-1   (0x4B constant)
```



其中，`**data block`是IO过程的基本单位。**换句话说，当你要读取外存`SSTable`key`时，其所在的`data block`将整个被读入内存并被解码。那如何定位该`key`所在的`data block`呢？答案是依赖Meta Section`，该字段中的`MetaEntry`记录了对应`data block`的元信息——偏移地址`offset`、第一个key的大小和第一个key的值、最后一个key的大小和最后一个key的值。

由于`data block`中的`key`是有序的，通过`[第一个key，最后一个key]`区间的边界信息，就能快速判断一个`是否有可能在该`data block`中，而`offset`则用于定位该`data block`在`Data Section`中索引位置，以便读入内存。

> data block是IO的基本单位，为了减少IO，后续还可以为data block设计一个缓冲池


### Block
上面提到的`data block`编码结构如下：

```cpp
每个data block的二进制编码格式：
---------------------------------------------------------------------------------------
|   block内的数据data部分     |       block内的Offset部分      |   block内的控制字段     |
|   (记作：block's Data)		 |     (记作：block's Offset)	 | (记作：block's Extra)   |
---------------------------------------------------------------------------------------
|Entry#1|Entry#2|...|Entry#N | Offset#1|Offset#2|...|Offset#N| num_of_elements | Hash |
----------------------------------------------------------------------------------------

-block's Data是一个Entry数组，Entry的大小不固定
其中的每个Entry均包含一个key-value的二进制编码信息：
---------------------------------------------------------------
|                           Entry #i                          |
----------------------------------------------------------------
|key_len (2B)|key(keylen)|val_len(2B)|val(vallen)|tranc_id(8B)|
---------------------------------------------------------------

-block's Offset中Offset则是对应Entry在block's Data的偏移地址（即数组的索引）
-block's Extra 中则是该data block的控制信息：num_of_elements（Entyr的个数），Hash（该data blco的hash码）

注意区分：
    这里的Offset是指data block中的Entry的偏移；Extra是该data block的控制信息
    而上面data Meta中的offset是SST中的data block的偏移；上面的Extra是SST整个文件的控制信息
```



代码中，外存二进制数据`data block`在内存中对应的是`Block`对象。`Block`的核心操作包含：

+ `Block对象---编码--->data block`
+ `data block---解码--->Block对象`

本代码设计中，`Block`只负责将内存中的二进制`block's Data`编码成`data block`；将外存`data block`中的`block's Data`取出来。Block中存放的始终二进制的数据`std::vector<uint8_t> data`，真正的编码和解码动作、文件读写动作其实在`SSTBuild`中。



1、`Block::encode()`

```cpp
// 计算block data总大小：数据部分的大小 + 偏移数组大小(每个偏移2字节) + 元素个数标志的大小(2字节)
size_t total_bytes = data.size() * sizeof(uint8_t) 
+ offsets.size() * sizeof(uint16_t) 
+ sizeof(uint16_t);
// +后续根据if(with_hash)，还可能加上索引字段大小：sizeof(uint32_t)

//data block的二进制表示
std::vector<uint8_t> encoded(total_bytes, 0);
// 依次将数据部分、偏移部分、控制信息部分放入encoded
memcpy(encoded.data(), block's Data部分的数据);
memcpy(encoded.data() + block's Data的size, block's Offset部分的数据);
memcpy(encoded.data() + block's Data的size + block's Offset的size, block's Extra部分的数据);

return encoded;
```



2、`Block::decode()`：由于每个Entry大小不固定，后续将二进制的`encoded`转为`key-value`需要偏移地址信息，所以`Block`中还需维护一个偏移地址数组`std::vector<uint16_t> offsets`

```cpp
auto block = std::make_shared<Block>();

// 先从block's Extra中获取元素个数
auto pos = encoded.size() - sizeof(uint16_t);//有索引的话，还有-sizeof(uint32_t)

uint16_t num_elements=0//block's Data中的Entry个数
memcpy(&num_elements, encoded.data() + num_elements_pos, sizeof(uint16_t));

// 获取block's Offset部分
memcpy(block->offsets.data(), block's Offset起始位置, num_elements * sizeof(uint16_t));

// 获取block's Data部分
block->data.reserve(block's Data的大小);  // 优化内存分配
block->data.assign(encoded.begin(), encoded.begin() + block's Offset起始位置);
```

### DataMeta
代码中，外存二进制数据`block meta`在内存中对应的是`DataMeta`对象。与`Block`类似，`DataMeta`的核心操作：

+ `DataMeta对象---编码--->block meta`
+ `block meta---解码--->DataMete对象`



`DataMeta`的定义：

```cpp
class BlockMeta {
public:
    BlockMeta();
    BlockMeta(size_t offset, const std::string& first_key,const std::string& last_key);
    // 将一个BlockMeta编码为一个block meta表示
    static void encode_meta_to_slice(std::vector<BlockMeta>& meta_entries, std::vector<uint8_t>& metadata);
    // 将整个data meta部分，解码成一组BlockMeta
    static std::vector<BlockMeta> decode_meta_from_slice(const std::vector<uint8_t>& metadata);

public:
    size_t offset;          // 块在文件中的偏移量
    std::string first_key;  // 块的第一个key
    std::string last_key;   // 块的最后一个key
};
```

### SSTBuilder
`SSTBuilder`是`SST`文件的构造器, 它将`MemTable`中的数据进行编码并写入磁盘形成`SST`文件。

**SST和SSTBuilder的关系**： `SSTBuilder对象`只在`SST文件`构建过程中存在, 构建过程可向`SSTBiilder`中添加`key-value`键值对，并最终在调用`Build()`后，将所有数据编码并写入为一个外存的`SST文件`， 同时构建一个对应的`SST对象`, `SST对象`是`SST文件`在内存中表现形式。

`SSTBuild`中的核心操作：

+ `SSTBuilder::add()`：向一个可写的`Block对象`中写入`key-value`
+ `SSTBuilder::finish_block()`：当可写的`Block对象`大小达到上限，则将该`Block对象`移入`SSTBuilder`中的`block_meta_vec`数组，并开启一个新的`Block对象`用于后续的数据写入
+ `SSTBuilder::build()`: 把所有 block + meta + footer **落盘**为外存的一个 `SST文件`，并返回内存中的对应表示`SST对象`

### SST
`SST`是外存的`SST文件`在内存中的表示，换句话说，对`SST文件`的访问都是通过操作`SST`的来间接实现的。`SST`的定义：

```cpp
class SST : public std::enable_shared_from_this<SST> {
private:
    FileObj file;	//对应的SST文件对象
    std::vector<BlockMeta> block_meta_vec;//对应编码中Meta Section部分的block meta数组
    uint32_t bloom_offset;	//对应编码中的Bloom Section部分
    uint32_t meta_block_offset;	//对应编码中Extra Section部分的meta_offset部分

    size_t sst_id;//该SST所在的level中的id号
    std::string first_key;	//该SSTable中第一个key(最小的key)
    std::string last_key;//该SSTable中最后一个key(最大的key)

    std::shared_ptr<BloomFilter> bloom_filter;

    std::shared_ptr<BlockCache> block_cache;    //Block的全局缓存池

    uint64_t min_tranc_id_ = UINT64_MAX;    //当前sst中最大的事务id
    uint64_t max_tranc_id_ = 0; //当前sst中最大的事务id

    // WiscKey fields
    uint8_t storage_mode_ = 0;  // 0=inline, 1=WiscKey
    std::shared_ptr<VLog> vlog_;

    ......
};
```



SST中的核心操作：

+ `SST::open()`：在初始化LSM引擎时，会将该LSM的所有`SST`进行数据加载（惰性加载，一开始仅仅加载`SST文件`的元数据和控制信息，真正的数据`data block`，会推迟到访问时才会进行加载）。
+ `SST::get()`：在`SST`中查询指定`key`
+ `SST::find_block_idx`：找到目标`key`可能会在该`SST`对象的哪个`Block`中
+ `SST::read_block()`：在找到需要访问的`Block` 索引后，将该`Block`读入内存（若使用了Block 缓存池，则先去缓存池中查找，没有找到才会触发IO）。
+ `SST::resolve_value()`: 若使用了key-value分离，则意味着查出来的value只是一个指针，还需通过`VLog`文件中取出真正的value



通过SST访问一个Block的流程：

```cpp
======================先初始化配置====================
LSMEngine::LSMEngine(){//创建LSMEngine对象时
    初始化日志: init_spdlog_file();
    
    初始化 block_cache;
    
    初始化 VLog;
    
    for(auto path : 所有SST文件路径的列表){
        //初始化SST对象，实际上仅仅是将SST文件中的元数据部分加载到了SST对象中
        // data block并不做加载，而是后续执行get()时，根据SST的元数据选择可能含有目标key的Block进行加载
        SST::open(path);
    }
}

=====================进行数据访问====================
LSMEngine::get
  → memtable 未命中
  → 候选 SST（L0 按新到旧 / L1+ 按 key 区间二分）
      → SST::get 
        → SstIterator::seek
      → SST::find_block_idx（在 block_meta_vec 上二分）
      → SST::read_block（BlockCache 命中？否则磁盘读取并decode 为 Block对象）
      → BlockIterator 构造
        → Block::get_idx_binary（block 内二分 + 事务可见性）
      → SST::resolve_value（WiscKey 时去vlog 取真实值）
```



### SstIterator
`SstIterator`的作用是实现对`SST`中元素的访问。先分析一下`SSTable`表的性质:

+ level-0层的各`SSTable`表是由`frozen_table`而来的，说明每张`SSTable`内的元素是有序的
+ level-1~level-n层的各`SSTable`表是由上层`SSTable`合并去重而来，也就是表内的**元素有序+key不重复**
+ 每层level的各SSTable，数据上可能有重复的key。按层进行访问时，应该如同上面Memtable中访问的方式一样，有去重和过滤无效id的手段。

****

SST访问数据时数据是惰性加载的，仅仅加载需要访问的那个`Block`。也就是说，对SST的访问，实际上是对所加载的Block的访问。所以`SstIterator`的底层，应该调用`BlockIterator`



`SstIterator::seek()`：上层的查询操作`get()`会通过`SST`中的 `std::shared_ptr<BlockIterator> m_block_it`来查找，而`SstIterator::seek()`的作用则是初始化这个`m_block_it`

```cpp
void SstIterator::seek(const std::string& key) {
    //查找key可能在的block索引
    m_block_idx = m_sst->find_block_idx(key);
    
    if (没找到合适的block_id) {  //说明这个key无法查询到
        m_block_it = nullptr;
        return ;
    }
    //能找到合适的block_id，则进行block的获取（要么从缓存池中拿、要么读盘）
    auto block = m_sst->read_block(m_block_idx);
    if (!block) {
        m_block_it = nullptr;
        return;
    }
    //根据获得的Block，构建指向key的BlockIterator，可能BlockIterator指向end
    m_block_it = std::make_shared<BlockIterator>(key);
}
```



## LSM引擎
### 压缩合并
上面完成了对`SST`的访问和落盘，但还需实现从`level`第`i`层到第`i+1`层的存储处理——压缩合并。本系统中的设计：

(1) 当`frozen_table的数量>设置的阈值`时，将`Memtable`中最旧的`frozen_table`落盘为`level-0`层的`SSTable`表。

(2) 若上述过程导致`level-0`层中`SSTable的数量>设置的阈值`，则进一步将`level-0`层所有`SSTable`以及`level-1`层中所有`SSTable`进行合并（全量合并），合并过程中进行去重并过滤无效的`key`，将处理后的结果按`level-1`中单张SSTable的大小进行划分，重新放入`level-1`层。

(3) `level-1`层也可能触发连锁反应，继续压缩合并放入`level-2`层，并以此类推，直到最终层中`SSTable`表的数量不满足压缩合并的条件为止。



由上可知，外存中`SSTable`的特点如下：

+ `level-0`的`SSTable`本质上是冻表的落盘，因此该层中的各`SSTable`表内、表间可能有重复的`key`
+ `level-1及以下层`的`SSTable`由全量压缩合并而来，该过程中有过滤动作，故这些层中的`SSTable`表间、表内无重复`key`。



在全量压缩合并的过程中，需要对本层以及上一层的数据进行访问，因此该过程涉及到若干迭代器类型的设计

+ `HeapIterator`：将若干跳表中的元素合并输出（仅level-0使用）
+ `ConcateIterator`：将若干SSTable中的元素归并（level-1及以下的层中使用）
+ `TwoMergeIterator`：对两个迭代器a和b中的值，按要求进行取值（要求是：key小的优先；同key则id大的优先）



### LSMEngine
上面已经实现了一系列的底层组件：SkipList、Memtable、SSTable、压缩合并机制以及各类迭代器。接下将对这些组件进行进一步的串联，初步实现一个KV存储引擎。

整个LsmEngine的作用：

+ 初始化整个系统、维护整个系统的重要信息
+ 串联一系列组件的调用
+ 整合Memtable表、SST表中迭代器，实现元素访问



### LSM Tree的优化
#### Block缓存池
`Block`是系统IO的基本单位，我们可以将最近一段时间内访问过的`Block`暂存在一个缓存池中，后续数据访问过程可以先去`Block`缓存池中寻找，若命中则可以减少IO的次数从而优化性能。

本系统Block缓存池采用的淘汰策略是：`LRU-K`策略

LRU-K = 看"倒数第 K 次访问时间"的LRU（Kth most recent access time）。但工程上，LRU-K 里的 K = "成为热点所需的访问次数阈值"。

访问 K 次 ⇔ 有第 K 次最近访问时间，即：如果`一个key访问 < K次`，则没有kth time；如果`一个key访问访问 ≥ K次`， 它才能参与`LRU`过程。工程上的实现：分为两个队列

1）队列probationary ：判断热点块（`访问次数<k`）

+ 被访问后，位置可以不变（很多实现中不做移动）    
+ 可能只更新“访问次数 / history”
+ 核心目标不是维护精确 LRU 顺序，只用于判断目标是否是热点数据

2）队列protected： 用 LRU 近似维护热点块集合（`访问次数>=k`）

+ 被访问后，一般会发生位置变化（通常 move-to-front）
+ 按“LRU（最近1次访问）”进行维护



#### 布隆过滤器
当`Block`缓存池未命中时，仍需要进行磁盘IO读取对应的`Block`，如果频繁未能命中，那就会发生多次IO读盘导致系统的性能降低。在设置布隆过滤器前，读`Block`的流程如下：

```cpp
在Memtable中查找失败;
for(auto sst : 所有SST){
    获取sst中的Block元数据;
    
    if(目标key in sst中某个data block的[first_key,last_key]){
        //仅仅是在该Block的区间范围内，是否真的存在还需读盘后进行判断
        if(该data block在Block缓存池中){
            从缓存池中取出Block;
        }else{
            将该data block从外存读入内存的Block;
        }

        auto value = 折半查找key-value数据数组;
        if(成功找到)
            return value;
    }
}
return 没有找到;
```



`SST`的元数据中记录者它所拥有的`data block`的`first_key`和`last_key`，但即使目标`key`包含于区间`[first_key,last_key]`，也无法保证目标的存在性，从而导致在不同的`SST`中发送读盘并尝试查找。

如果有一种手段可以直接判断目标key在`SST`中的存在性，那不就可以大大减少读盘次数了吗？实际上，这就是布隆过滤器bloom_filter的作用。

<img src="https://cdn.nlark.com/yuque/0/2026/png/28455902/1786890658188-d7a64b8d-9680-4b78-9a7e-a463837f90c1.png" width="285" title="" crop="0,0,1,1" id="ub073b777" class="ne-image">



bloom_filter的机制及原理：

+ 包含：一张位图`bits_`，多个不同的`hash函数`
+ `put(key,value)`阶段，key会经过`m`个`hash函数`得到多个索引号`idx<sub>0,</sub>idx<sub>1</sub>，..., idex<sub>m</sub>`，将位图`bits_`中的索引号`idx<sub>0,</sub>idx<sub>1</sub>，..., idex<sub>m</sub>`全部设置为1。`get(key)`阶段，先通过同样的`hash函数`计算出索引号，在位图`bits_`中查看这些索引位置上的数字是否全是1；如果不是，则`key`一定不存在；如果是，则`key`可能存在（由于哈希冲突，可能导致误判）

于是处理流程由

```cpp
if(该data block在Block缓存池中){
    从缓存池中取出Block;
}else{
    将该data block从外存读入内存的Block;
}
```

优化为了：

```cpp
if(该data block在Block缓存池中){
    从缓存池中取出Block;
}else if(目标key在bloom_filter中命中){ //可以过滤掉了大部分无效的IO读盘
    将该data block从外存读入内存的Block;
}
```

### MVCC&事务&WAL
#### MVCC
多版本并发控制（MVCC）是数据存储中的一种"以空间换时间"方案。通过为同一数据维护多个版本，使不同事务能够在读数据时，读各自想要的数据版本。这样能减少读写之间的阻塞，从而减少事务之间锁的使用，提高并发性能。

本系统中的简易版MVCC：

+ 可存储同`key`的不同版本（版本用`tranc_id`区分）
+ 访问时，可以通过指定`tranc_id`来限制`get()`操作中可见的`key`版本。
    - 若未指定`tranc_id`，则`tranc_id=0`（默认），表示可见所有版本
    - 操作只能访问`tranc_id >= id`中的最新的`key`


该系统中的MVCC功能的是一个极简版本。  
	MVCC的优势是：不同事务并发访问同一个key的不同版本时，可以无需加锁。例如：对同key不同版本并发进行读和写无需加锁（这是它的最大优势）。  
	但本系统中的MVCC并未实现，仅仅实现了多版本数据的管理和使用，访问过程依然使用了共享锁。


#### 事务
##### **事务介绍**
事务是一组操作的集合，遵循ACID原则。ACID 是**数据库事务的四个基本特性**：

| 特性 | 含义 | 简单理解 |
| --- | --- | --- |
| **Atomicity 原子性** | 事务中的操作要么全部成功，要么全部失败 | **要么全做，要么全不做** |
| **onsistency 一致性** | 事务执行前后，数据库都必须满足约束和规则 | **数据始终合法** |
| **Isolation 隔离性** | 并发事务之间互不干扰 | **事务之间尽量看不到彼此的中间状态** |
| **Durability 持久性** | 事务提交后，数据永久保存，即使发生故障也不能丢失 | **提交后就不会轻易丢失** |


##### 事务的实现
由于`KV`存储的数据形式非常简单, 不存在类似关系型数据库中的外键、触发器、声明式约束等复杂业务规则, 因此对**一致性**无需特意去做实现。而对其他事务特性，本系统的实现如下：

+ **原子性**：先将事务的一系列操作`Record`写入本事务对象内的一个缓存区, 在commit时统一执行
+ **隔离性**：涉及“读已提交”和“可重复读”
+ **持久性**：当事务commit时，操作缓存区中的所有 `Record`会被写入`WAL`文件中，预先实现持久化，随后才真正执行`Memtable`的写入



**原子性实现**

在事务`TranContext`对象中设置一个操作缓存数组`std::vector<Record> operations`，事务执行过程中，不会真的执行`put`操作，而是往该数组中存放所做的`put`指令，同时将所有的`put`操作结果key-value存入一个成员变量`wirte_map_`。  
	事务`commits`时，先将`operations`中所有操作写入`WAL`文件，随后才执行`Memtable.put()`使数据进入数据库。（写`WAL`的做法有点类似于redis中的写增量日志的做法**AOF** ）

`TranContext::put()`

```cpp
void TranContext::put() {
    // 所有隔离级别都需要先写入 operations 中
    operations.emplace_back(Record::putRecord(this->tranc_id_, key, value));
    // 暂存到 wirte_map_ 中, 统一提交后才在数据库中生效
    wirte_map_[key] = value;
}
```



`TranContext::commit()`

```cpp
检查commit是否有冲突;
......
    
//获取提交版本号committed_seq_
auto committed_seq = get_next_global_seq();
//给所有操作中的key设置版本号
 for(auto& record : operations){
    record.setTrancid(committed_seq);
}

// 事务的提交（预先写入WAL文件中，使之持久化）
operations.emplace_back(Record::commitRecord(committed_seq));
if (!write_to_wal(operations)) {
    若提交过程出问题，进行错误处理;
}

//写入Memtable中
for (auto& [k, v] : wirte_map_) {
    // 提交给memtable使用的版本号 与 提交给 WAL时使用的版本号相同
    memtable.put_(k, v, committed_seq);
}
```



**隔离性与持久性实现**

+ **"读已提交"的实现**

“读已提交”和持久化本质上是同一个问题，因为”数据持久化“ 等同于 ”数据提交生效“。  
	使用`WAL`文件前，`put()`操作将一个数据放入`Memtable`中，此时尚未落盘，故对`Memtable`中的数据而言还算不上`"已提交"`的状态。而使用了`WAL`文件后，`commit`时先触发`WAL`的写入（持久化），随后才进行`put`写入`Memtable`，这样一来就达成了事务提交的“持久化”目标，此后从`Memtable`中读取的数据也就成了真正的“已提交状态”了。  



+ **"可重复读"的实现**

> 本系统中：RR的提交 = 内存快照（Snapshot） 保证重复读 + W-W 检测 ** **
>

对于隔离级别为“可重复读”的事务，我们的做法是：在事务`TranContext`对象中设置一个成员变量`read_map_`作为缓存区，首次读取的key会存入该缓存区中作为快照；同时，事务执行过程中，所有的写操作结果key-value也会存入一个成员变量`wirte_map_`。后续的非首次访问就直接从这两个缓存区中获取，这样一来就保证了数据读取的可重复性

`TranContext::get()`

```cpp
事务创建时，获取当前最新的tranc_id;
// 先尝试在写缓存中查询目标key
if(wirte_map_.find[key] != wirte_map_.end()){
    return wirte_map_[key];
}
// 上述过程未找到，再尝试在读缓存中查询目标key
if(read_map_.find[key] != read_map_.end()){
    return read_map_[key];
}
// 写缓存、读缓存均没有找到，说明这个key是首次访问
auto &[key, value] = get(key,tranc_id);
read_map_[key] = [value];//首次访问的key记录至读缓存
return value;
```

对“可重复读”的事务在commit时，我们还要进行如下W-W冲突检查。

`TranContext::commit()`

```cpp
事务准备进行提交前，分配一个committed_seq;
//进行W-W检查
for (auto& [k, v] : wirte_map_) {
    auto last_tranc_id = get(k,0)//获取系统当前最新版本key的版本号tranc_id

    if(last_tranc_id > committed_seq){
        说明有一个事务写入了一个更新的版本，属于W-W冲突;
        return false;
    }  
}
......
执行commit动作;
......
return true;
```

#### WAL预写式日志
##### **崩溃恢复**
**场景**：系统因故障崩溃停止，导致`Memtable`中的数据丢失。

**处理**：系统重新启动时，需要用到以下两个文件：

+ `tranc_info_file`文件：获取崩溃前已分配的事务最大id值:`global_seq_`和已落盘的事务id值：`max_flushed_seq_`
+ `WAL`文件：记录了事务对应的一系列`Record`，可用来恢复`Memtable`的数据



`LSM::recover_from_wal()``-->TranManager::check_recover()-->WAL::recover()`

```cpp
系统启动时，执行LSM::recover_from_wal()
--> 从tranc_info_file恢复global_seq_和max_flushed_seq_
-->TranManager::check_recover()
    -->WAL::recover(){
        读取WAL文件路径的数组wal_paths;
        for(auto path : wal_paths){
            //打开一个wal文件
            auto wal_file = open(paht);
            // 从该wal文件中解码出所有Record
            auto records = Record::decord(wal_file);
            for (const auto &record : records) {
                // Record的tranc_id 大于 max_flushed_seq, 说明这条记录没有落盘，需要恢复
                if (record的committed_seq > max_flushed_seq) {
                    tranc_records[committed_seq].push_back(record);
                }
            }
        }
    }
```

##### **残缺WAL文件**
**【问题】**`Record`写到一半崩溃留，此时`WAL`尾部的`Record`是残缺的 → 重启系统，由于WAL文件中的有残缺的`Record`，导致数据恢复过程出错，服务无法启动。

**处理**：

+ 恢复时跳过残缺的事务，从而让服务正常启动，不会崩溃循环。（一个完整的事务最后一定是`OperationType::OP_COMMIT`，这可作为`WAL`中完整事务`Record`集的边界）。
+ 每次重启系统，都新建一个`WAL`文件用于本次`Record`的写入（目的是以防上轮`WAL`中的残缺数据干扰本轮的写入）  

##### **后台清理**
**场景**：随着系统运行时间的累计，写入的`WAL`文件会越来越多，落盘的数据也越来越多。对早期的`WAL`文件而言，若其中的数据已经全部持久化到了`SST`文件中，那该文件就没有存在的必要了

**处理**：在系统初始化时，会启动一个后台线程，每隔一段清理过期的`WAL`文件

```cpp
while(true){
    if(!stop_clearner) break;
        
    读取WAL文件路径的数组wal_paths;
    ......
    // 判断是否过期，若过期则删除
    for(auto path: wal_pahts){
        auto wal_file = open(path);
        // 是否有未落盘的数据（Record）
        bool has_unflushed = false;
        while(从wal_file中依次取出Record){
            if(该Record的committed_seq > max_flushed_seq){
                has_unflushed = true;
                break;
            }
        }
        // 若所有Record均已落盘，说明本WAL内容已经过期，可以清理
        if(!has_unflushed){
            del_paths.push_back(std::move(wal_file));
        } 
    } 

    清理del_paths中所记录的已过期WAL文件;
}
```

### WiscKey 键值分离
#### 键值分离的实现
每次`Compact`，系统需要把多个`SST`文件读入内存，归并排序后再写回磁盘。这个“读 + 重写“的过程对`key` 来说是必要的（因为要合并多个版本、消除重复），但对 `value`来说却是冗余开销——`value` 本身并没有发生任何变化，但它随着 `key`被反复读写。

`Compact`过程中，若`value`的所占的字节数较大，就会导致大量的 I/O 时间用于数据的搬运与合并，从而拖累系统的写入效率



**处理思路**：

既然`value`对`compact`而言是不相关的，那就可以将`key`和`value`分开存储。`SST`中仅存储`key`和一个`12B的value地址编码结构`，`value`的本体则写入一个独立的顺序日志文件`VLog`（`Value Log`）。这样一来，`Compact`重写时仅包括`key`+`12B的value地址编码结构`，而`value`的本体则在 `VLog`中原地不动，使得重写的数据量减少，从而缓解写放大问题。

```cpp
一个典型的 value 地址编码结构 可以表示为：
┌──────────────┬──────────────┬──────────────┐
│  FileID      │   Offset     │    Length    │
│    4B        │     4B       │      4B      │
└──────────────┴──────────────┴──────────────┘
FileID：标识Vlog文件
Offset：表示该value在Vlog中的偏移位置
Length：表示value的字长，通过Offset+Length就能获知该value在对应Vlog的数据部分
所以是：4B FileID + 4B Offset + 4B Length = 12B

而Vlog中的真实value编码：Entry
[key_len:2B][key:key_len][val_len:4B][value:val_len][crc32:4B]
```

#### Vlog文件的GC机制
同`WAL`文件类似，`Vlog`中记录的`value`值也有可能过期，因此同样需要清理机制，**但该部分目前还未实现。**

我目前的思路是：`key`的删除和去重操作是在`level-0~n`层的压缩合并过程中出现的，这也是导致`Vlog`中数据失效的根源。我们可以在`SST`压缩合并的过程中先记录下要过滤掉`key`，在合并完成后再对`Vlog`的做清理工作，并更新`SST`中的`value`的地址编码。

### Redis协议支持
`RESP`是Redis定义的一套客户端与服务器之间的数据序列化/通信协议。用第一个特殊字符表示数据类型，后面跟数据长度/内容，最后以`\r\n` 结束。

**RESP的 5 种格式**

| 类型 | 格式 | 示例 |
| :---: | :---: | :---: |
| Simple String | `+内容\r\n` | `+OK\r\n` |
| Error | `-错误信息\r\n` | `-ERR error\r\n` |
| Integer | `:数字\r\n` | `:100\r\n` |
| Bulk String | `$长度\r\n内容\r\n` | `$3\r\nGET\r\n` |
| Array | `*元素个数\r\n元素...` | `*2\r\n$3\r\nGET\r\n$4\r\nname\r\n` |


**设置过期时间的实现和使用**

```cpp
// 设置过期时间
epire(key, expire_time){
    // 先存储key-过期时间
    lsm.put("REDIS_EXPIRE_"+key, expire_time);
}

// 读元素前先检查过期时间
get(key){
    // 先查询过期时间
    auto expire_time = lsm.get("REDIS_EXPIRE_"+key);
    
    if(expire_time && expire_time < 当前时间){
        return 过期了;
    }else{
        auto value = lsm.get(key);
        reutrn value;
    }
}
```



**对Redis中hash类型的支持**：以HSET命令为例

```cpp
/*  
例如命执行令：
    HSET user name 张三 age 20 sex male
LSM中将存入：
    user       		       → "$name$age$male"
    REDIS_FIELD_user_name  → "张三"
    REDIS_FIELD_user_age   → "20" 
    REDIS_FIELD_user_sex   → "male" 
*/
// std::vector<std::string> &args是RESP字符串解析后的数组

// 指令字段，例如：put, get等
const std::string &key = args[1];
// 参数字段，例如：key、value、expire等
for (size_t i = 2; i < args.size(); i += 2) {
    if (i + 1 >= args.size()) 
            break;  // 防止越界
    fields.emplace_back(args[i]);
    fieldValues.emplace_back(args[i + 1]);
}
lsm.put("REDIS_FIELD_"+key+"_" + fileds[0], fieldValues[0]);
lsm.put("REDIS_FIELD_"+key+"_" + fileds[1], fieldValues[1]);
lsm.put("REDIS_FIELD_"+key+"_" + fileds[2], fieldValues[2]);

// 将fileds数组转为"$name$age$male"字符串
auto filed_str = get_hash_value_from_fields(fileds)
lsm.put(key,filed_str)
```

****

**对Redis中list类型的支持**：以LPUSH命令为例

```cpp
/*
例如命执行令：会在“列表左侧插入新元素”
    LPUSH mylist A
    LPUSH mylist B
    LPUSH mylist C

LSM中将存入:
    mulist  → "#A#B#C" 
*/
const std::string &key = args[1];
auto list_value = lsm->get(key);
if (!list_value.empty()) {
    list_value = value + "#" + list_value;
} else {
    list_value = value;
}
```

****

**对Redis中set（无序集合）类型的支持**：以SADD为例

```cpp
/*
例如命执行令： 
    SADD fruits apple banana
LSM中将存入：
    fruits 						→"2" (集合中元素的个数)
    REDIS_SET_fruits_apple		→"1"
    REDIS_SET_fruits_banana		→"1"
*/
```

****

**对Redis中zset（有序集合）类型的支持**：以操作ZADD为例

```cpp
/*
例如命执行令： 
    ZADD ranking 100 Tom

LSM中将存入：
    ranking 								→"REDIS_SORTED_SET_ranking_"
    REDIS_SORTED_SET_ranking_SCORE_100  	→"Tom"
    REDIS_SORTED_SET_ranking_ELEM_Tom		→"100"

后续查询时，会通过ranking查询到REDIS_SORTED_SET_ranking_，然后通过在LSM中进行前缀查询
*/
```
