/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/* Return the required encoding for the provided value. */
static uint8_t _intsetValueEncoding(int64_t v)
{
    if (v < INT32_MIN || v > INT32_MAX)
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;
}

/* Return the value at pos, given an encoding. */
static int64_t _intsetGetEncoded(intset* is, int pos, uint8_t enc)
{
    int64_t v64;
    int32_t v32;
    int16_t v16;

    // memcpy 好象是内存拷贝
    // 第一个参数是 dest(目的地)
    // 第二个参数是 src
    // 第三个参数是 length

    if (enc == INTSET_ENC_INT64)
    {
        // 强转为 int64_t 这样偏移量就是 + pos * sizeof(int64_t)
        memcpy(&v64, ((int64_t*)is->contents) + pos, sizeof(v64));
        memrev64ifbe(&v64);
        return v64;
    }
    else if (enc == INTSET_ENC_INT32)
    {
        memcpy(&v32, ((int32_t*)is->contents) + pos, sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    }
    else
    {
        memcpy(&v16, ((int16_t*)is->contents) + pos, sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}

/* Return the value at pos, using the configured encoding. */
static int64_t _intsetGet(intset* is, int pos)
{
    return _intsetGetEncoded(is, pos,intrev32ifbe(is->encoding));
}

/**
 * Set the value at pos, using the configured encoding.
 *
 * 简单理解就是数组赋值
 *
 * @param is  intset 指针
 * @param pos  contents 数组下标
 * @param value  value 元素
 */
static void _intsetSet(intset* is, int pos, int64_t value)
{
    uint32_t encoding = intrev32ifbe(is->encoding);

    // 赋值到数组中，然后如果是大端存储，就翻转字节

    if (encoding == INTSET_ENC_INT64)
    {
        ((int64_t*)is->contents)[pos] = value;
        memrev64ifbe(((int64_t*)is->contents)+pos);
    }
    else if (encoding == INTSET_ENC_INT32)
    {
        ((int32_t*)is->contents)[pos] = value;
        memrev32ifbe(((int32_t*)is->contents)+pos);
    }
    else
    {
        ((int16_t*)is->contents)[pos] = value;
        memrev16ifbe(((int16_t*)is->contents)+pos);
    }
}

/* Create an empty intset. */
intset* intsetNew(void)
{
    intset* is = zmalloc(sizeof(intset));
    is->encoding = intrev32ifbe(INTSET_ENC_INT16);
    is->length = 0;
    return is;
}

/**
 * Resize the intset
 *
 * 调整 intset 结构体大小。
 *
 * 为什么知道了长度就可以分为大小了呢？因为 encoding
 *
 * @param is intset 指针
 * @param len 新的长度 len
 * @return
 */
static intset* intsetResize(intset* is, uint32_t len)
{
    // 计算 intset 最后一个数组占用多少字节
    uint32_t size = len * intrev32ifbe(is->encoding);

    // 分配一个新的空间，除了 sizeof 还需要分配数组空间
    is = zrealloc(is, sizeof(intset) + size);
    return is;
}

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
static uint8_t intsetSearch(intset* is, int64_t value, uint32_t* pos)
{
    int min = 0, max = intrev32ifbe(is->length) - 1, mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (intrev32ifbe(is->length) == 0)
    {
        if (pos) *pos = 0;
        return 0;
    }
    else
    {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        // 比最大值还大，肯定找不到了
        if (value > _intsetGet(is,intrev32ifbe(is->length) - 1))
        {
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;
        }

        // 比最小值还小，也找不到
        else if (value < _intsetGet(is, 0))
        {
            if (pos) *pos = 0;
            return 0;
        }
    }

    while (max >= min)
    {
        // 二分查找
        // (min + max) / 2 = min
        mid = ((unsigned int)min + (unsigned int)max) >> 1;
        cur = _intsetGet(is, mid);
        if (value > cur)
        {
            min = mid + 1;
        }
        else if (value < cur)
        {
            max = mid - 1;
        }
        else
        {
            // 相等找到，min ！
            break;
        }
    }

    // 找到就返回
    if (value == cur)
    {
        if (pos) *pos = mid;
        return 1;
    }
    else
    {
        if (pos) *pos = min;
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
static intset* intsetUpgradeAndAdd(intset* is, int64_t value)
{
    // 之前的编码
    uint8_t curenc = intrev32ifbe(is->encoding);
    // 要升级的编码
    uint8_t newenc = _intsetValueEncoding(value);

    // 获取多少个需要升级的元素
    int length = intrev32ifbe(is->length);
    int prepend = value < 0 ? 1 : 0;

    /* First set new encoding and resize */
    // 新的编码直接赋值进去
    is->encoding = intrev32ifbe(newenc);

    // intsetResize
    // 为什么要 + 1？预留 新元素 空间
    is = intsetResize(is,intrev32ifbe(is->length) + 1);

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */

    // 这里其实破坏了 length 的语义，将 length 作为了指针，而且是从后往前的指针
    // 从后往前是唯一选择，类似于将元素往后移动而已
    while (length--)
        // _intsetGetEncoded 将数值扩展拷贝一份
        // 然后给数组复制
        _intsetSet(is, length + prepend, _intsetGetEncoded(is, length, curenc));

    // 当原来所有的元素都迁移完毕，接下来就是将 新的元素 放到恰当的位置

    /* Set the value at the beginning or the end. */
    // 数据插入在开头还是结尾
    if (prepend)
        _intsetSet(is, 0, value);
    else
        _intsetSet(is,intrev32ifbe(is->length), value);

    // 设置 length
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

static void intsetMoveTail(intset* is, uint32_t from, uint32_t to)
{
    void *src, *dst;
    uint32_t bytes = intrev32ifbe(is->length) - from;
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64)
    {
        src = (int64_t*)is->contents + from;
        dst = (int64_t*)is->contents + to;
        bytes *= sizeof(int64_t);
    }
    else if (encoding == INTSET_ENC_INT32)
    {
        src = (int32_t*)is->contents + from;
        dst = (int32_t*)is->contents + to;
        bytes *= sizeof(int32_t);
    }
    else
    {
        src = (int16_t*)is->contents + from;
        dst = (int16_t*)is->contents + to;
        bytes *= sizeof(int16_t);
    }
    memmove(dst, src, bytes);
}

/**
 * Insert an integer in the intset
 * @param is 整数集合指针
 * @param value 插入的值
 * @param success
 * @return
 */
intset* intsetAdd(intset* is, int64_t value, uint8_t* success)
{
    // 判断插入值的 encoding
    // valenc -> value encoding
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    // 如果插入数值的编码比 intset 编码还要大，那么需要升级
    // intrev32ifbe 只是一个字节序大端小端的问题
    if (valenc > intrev32ifbe(is->encoding))
    {
        /* This always succeeds, so we don't need to curry *success. */
        return intsetUpgradeAndAdd(is, value);
    }
    else
    {
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        if (intsetSearch(is, value, &pos))
        {
            if (success) *success = 0;
            return is;
        }

        is = intsetResize(is,intrev32ifbe(is->length) + 1);
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is, pos, pos + 1);
    }

    _intsetSet(is, pos, value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

/* Delete integer from intset */
intset* intsetRemove(intset* is, int64_t value, int* success)
{
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 0;

    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is, value, &pos))
    {
        uint32_t len = intrev32ifbe(is->length);

        /* We know we can delete */
        if (success) *success = 1;

        /* Overwrite value with tail and update length */
        if (pos < (len - 1)) intsetMoveTail(is, pos + 1, pos);
        is = intsetResize(is, len - 1);
        is->length = intrev32ifbe(len-1);
    }
    return is;
}

/* Determine whether a value belongs to this set */
uint8_t intsetFind(intset* is, int64_t value)
{
    uint8_t valenc = _intsetValueEncoding(value);
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is, value,NULL);
}

/* Return random member */
int64_t intsetRandom(intset* is)
{
    return _intsetGet(is, rand() % intrev32ifbe(is->length));
}

/* Sets the value to the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
uint8_t intsetGet(intset* is, uint32_t pos, int64_t* value)
{
    if (pos < intrev32ifbe(is->length))
    {
        *value = _intsetGet(is, pos);
        return 1;
    }
    return 0;
}

/* Return intset length */
uint32_t intsetLen(intset* is)
{
    return intrev32ifbe(is->length);
}

/* Return intset blob size in bytes. */
size_t intsetBlobLen(intset* is)
{
    return sizeof(intset) + intrev32ifbe(is->length) * intrev32ifbe(is->encoding);
}

#ifdef INTSET_TEST_MAIN
#include <sys/time.h>

void intsetRepr(intset *is) {
    int i;
    for (i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

void error(char *err) {
    printf("%s\n", err);
    exit(1);
}

void ok(void) {
    printf("OK\n");
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

#define assert(_e) ((_e)?(void)0:(_assert(#_e,__FILE__,__LINE__),exit(1)))
void _assert(char *estr, char *file, int line) {
    printf("\n\n=== ASSERTION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n",file,line,estr);
}

intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t i, value;
    intset *is = intsetNew();

    for (i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

void checkConsistency(intset *is) {
    int i;

    for (i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

int main(int argc, char **argv) {
    uint8_t success;
    int i;
    intset *is;
    sranddev();

    printf("Value encodings: "); {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) == INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
    }

    printf("Large number of random adds: "); {
        int inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",num,size,usec()-start);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
    }
}
#endif
