/*
Copyright (c) 2012-2014 The SSDB Authors. All rights reserved.
Use of this source code is governed by a BSD-style license that can be
found in the LICENSE file.
*/
#include <limits.h>
#include "../include.h"
#include "ssdb_impl.h"


static int zset_one(SSDBImpl *ssdb, leveldb::WriteBatch &batch, const Bytes &name, const Bytes &key, double score, uint16_t version);

static int zdel_one(SSDBImpl *ssdb, leveldb::WriteBatch &batch, const Bytes &name, const Bytes &key, uint16_t version);

static int incr_zsize(SSDBImpl *ssdb, leveldb::WriteBatch &batch, const ZSetMetaVal &zv, const Bytes &name, int64_t incr);

/**
 * @return -1: error, 0: item updated, 1: new item inserted
 */

int SSDBImpl::multi_zset(const Bytes &name, const std::map<Bytes ,Bytes> &sortedSet, int flags) {
    RecordLock l(&mutex_record_, name.String());
    return zsetNoLock(name, sortedSet, flags);
}

int SSDBImpl::multi_zdel(const Bytes &name, const std::set<Bytes> &keys) {
    RecordLock l(&mutex_record_, name.String());
    return zdelNoLock(name, keys);
}

int zsetAdd(SSDBImpl *ssdb, leveldb::WriteBatch &batch, bool needCheck,
             const Bytes &name, const Bytes &key, double score, uint16_t cur_version,
            int *flags, double *newscore) {
    /* Turn options into simple to check vars. */
    int incr = (*flags & ZADD_INCR) != 0;
    int nx = (*flags & ZADD_NX) != 0;
    int xx = (*flags & ZADD_XX) != 0;
    *flags = 0; /* We'll return our response flags. */

    /* NaN as input is an error regardless of all the other parameters. */
    if (std::isnan(score)) {
        *flags = ZADD_NAN;
        return NAN_SCORE;
    }
    else if (score < ZSET_SCORE_MIN || score > ZSET_SCORE_MAX){
        return ZSET_OVERFLOW;
    }

    if (needCheck) {
        double old_score = 0;

        std::string dbkey = encode_zset_key(name, key, cur_version);
        int found = ssdb->GetZSetItemVal(dbkey, &old_score);
        if (found < 0) {
            return found;
        }

        if (found == 0) {
            if (!xx) {
                if (newscore) *newscore = score;
                zset_one(ssdb, batch, name, key, score, cur_version);
                *flags |= ZADD_ADDED;
            } else {
                *flags |= ZADD_NOP;
            }

            return 1;

        } else if (found == 1) {
            if (nx) {
                *flags |= ZADD_NOP;
                return 1;
            }

            if (incr) {
                score += old_score;
                if (std::isnan(score)) {
                    *flags |= ZADD_NAN;
                    return NAN_SCORE;
                }
                else if (score < ZSET_SCORE_MIN || score > ZSET_SCORE_MAX){
                    return ZSET_OVERFLOW;
                }
                if (newscore) *newscore = score;
            }

            if (fabs(old_score - score) < eps) {
                //same
            } else {
                if (newscore) *newscore = score;

                string old_score_key = encode_zscore_key(name, key, old_score, cur_version);
                batch.Delete(old_score_key);
                zset_one(ssdb, batch, name, key, score, cur_version);
                *flags |= ZADD_UPDATED;
            }
            return 1;
        } else {
            //error
            return -1;
        }
    } else {

        if (!xx) {
            if (newscore) *newscore = score;
            zset_one(ssdb, batch, name, key, score, cur_version);
            *flags |= ZADD_ADDED;
        } else {
            *flags |= ZADD_NOP;
        }

        return 1;
    }
}

int SSDBImpl::zsetNoLock(const Bytes &name, const std::map<Bytes ,Bytes> &sortedSet, int flags) {
    int incr = (flags & ZADD_INCR) != 0;
    int nx = (flags & ZADD_NX) != 0;
    int xx = (flags & ZADD_XX) != 0;
    int ch = (flags & ZADD_CH) != 0;

    /* XX and NX options at the same time are not compatible. */
    if (nx && xx) {
        return SYNTAX_ERR;
    }

    //INCR option supports a single increment-element pair
    if (incr && sortedSet.size() > 1) {
        return SYNTAX_ERR;
    }

    /* The following vars are used in order to track what the command actually
 * did during the execution, to reply to the client and to trigger the
 * notification of keyspace change. */
    int added = 0;      /* Number of new elements added. */
    int updated = 0;    /* Number of elements with updated score. */
    int processed = 0;  /* Number of elements processed, may remain zero with
                           options like XX. */


    leveldb::WriteBatch batch;

    ZSetMetaVal zv;
    std::string meta_key = encode_meta_key(name);
    int ret = GetZSetMetaVal(meta_key, zv);
    uint16_t cur_version = 0;
    bool needCheck = false;

    if (ret < 0) {
        return ret;
    } else if (ret == 0) {
        needCheck = false;
        if (zv.del == KEY_DELETE_MASK) {
            if (zv.version == UINT16_MAX){
                cur_version = 0;
            } else{
                cur_version = (uint16_t) (zv.version + 1);
            }
        } else {
            cur_version = 0;
        }
    } else {
        needCheck = true;
        cur_version = zv.version;
    }

    double newscore;

    for(auto const &it : sortedSet)
    {
        const Bytes &key = it.first;
        const Bytes &val = it.second;

//        log_info("%s:%s" , hexmem(key.data(),key.size()).c_str(), hexmem(val.data(),val.size()).c_str());

        double score = val.Double();

        if (score > ZSET_SCORE_MAX || score < ZSET_SCORE_MIN) {
            return -1;
        }
        int retflags = flags;

        int retval = zsetAdd(this, batch ,needCheck, name, key, score, cur_version, &retflags, &newscore);
        if (retval < 0) {
            return retval;
        }

        if (retflags & ZADD_ADDED) added++;
        if (retflags & ZADD_UPDATED) updated++;
        if (!(retflags & ZADD_NOP)) processed++;
    }

    if (added > 0) {
        if (int rett = incr_zsize(this, batch, zv, name, added) < 0) {
            log_error("incr_zsize error");
            return rett;
        }
    }

    leveldb::Status s = ldb->Write(leveldb::WriteOptions(), &(batch));
    if (!s.ok()) {
        log_error("zset error: %s", s.ToString().c_str());
        return STORAGE_ERR;
    }

    if (ch) {
        return added+updated;
    } else {
        return added;
    }
}

int SSDBImpl::zdelNoLock(const Bytes &name, const std::set<Bytes> &keys) {
    ZSetMetaVal zv;
    leveldb::WriteBatch batch;

    std::string meta_key = encode_meta_key(name);
    int ret = GetZSetMetaVal(meta_key, zv);
    if (ret <= 0) {
        return ret;
    }

    int count = 0;
    for (auto it = keys.begin(); it != keys.end() ; ++it) {
        const Bytes &key = *it;
        ret = zdel_one(this, batch, name, key, zv.version);
        if (ret < 0) {
            return ret;
        }
        count += ret;
    }

    if (count > 0) {
        ret = incr_zsize(this, batch, zv, name, -count);
        if (ret < 0) {
            return ret;
        }

        leveldb::Status s = ldb->Write(leveldb::WriteOptions(), &(batch));
        if (!s.ok()) {
            log_error("zdel error: %s", s.ToString().c_str());
            return STORAGE_ERR;
        }
    }

    return count;
}

int64_t SSDBImpl::zcount(const Bytes &name, const Bytes &score_start, const Bytes &score_end) {
    int64_t count = 0;

    ZSetMetaVal zv;
    const leveldb::Snapshot* snapshot = nullptr;

    {
        RecordLock l(&mutex_record_, name.String());
        std::string meta_key = encode_meta_key(name);
        int res = GetZSetMetaVal(meta_key, zv);
        if (res <= 0) {
            return res;
        }

        snapshot = ldb->GetSnapshot();
    }

    SnapshotPtr spl(ldb, snapshot); //auto release

    auto it = std::unique_ptr<ZIterator>(this->zscan_internal(name, "", score_start, score_end, INT_MAX, Iterator::FORWARD, zv.version, snapshot));
    while(it->next()){
        count ++;
    }

    return count;
}

int SSDBImpl::zincr(const Bytes &name, const Bytes &key, double by, int flags, double *new_val) {
    RecordLock l(&mutex_record_, name.String());
    leveldb::WriteBatch batch;
    ZSetMetaVal zv;
    uint16_t cur_version = 0;
    bool needCheck = false;

    std::string meta_key = encode_meta_key(name);
    int ret = GetZSetMetaVal(meta_key, zv);
    if (ret < 0) {
        return ret;
    } else if (ret == 0) {
        needCheck = false;
        if (zv.del == KEY_DELETE_MASK) {
            if (zv.version == UINT16_MAX){
                cur_version = 0;
            } else{
                cur_version = (uint16_t) (zv.version + 1);
            }
        } else {
            cur_version = 0;
        }
    } else {
        needCheck = true;
        cur_version = zv.version;
    }

    int retflags = flags;
    int retval = zsetAdd(this, batch ,needCheck, name, key, by, cur_version, &retflags, new_val);
    if (retval < 0) {
        return retval;
    }

    if (retflags & ZADD_ADDED) {
        ret = incr_zsize(this, batch, zv, name, 1);
        if (ret < 0) {
            return ret;
        }
    }
    leveldb::Status s = ldb->Write(leveldb::WriteOptions(), &(batch));
    if (!s.ok()) {
        log_error("zset error: %s", s.ToString().c_str());
        return STORAGE_ERR;
    }

    return 1;
}

int64_t SSDBImpl::zsize(const Bytes &name) {
    ZSetMetaVal zv;
    std::string size_key = encode_meta_key(name);
    int ret = GetZSetMetaVal(size_key, zv);
    if (ret <= 0) {
        return ret;
    } else {
        return (int64_t) zv.length;
    }
}

int SSDBImpl::GetZSetMetaVal(const std::string &meta_key, ZSetMetaVal &zv) {
    std::string meta_val;
    leveldb::Status s = ldb->Get(leveldb::ReadOptions(), meta_key, &meta_val);
    if (s.IsNotFound()) {
        zv.length = 0;
        return 0;
    } else if (!s.ok() && !s.IsNotFound()) {
        return STORAGE_ERR;
    } else {
        int ret = zv.DecodeMetaVal(meta_val);
        if (ret < 0) {
            return ret;
        } else if (zv.del == KEY_DELETE_MASK) {
            return 0;
        } else if (zv.type != DataType::ZSIZE){
            return WRONG_TYPE_ERR;
        }
    }
    return 1;
}

int SSDBImpl::GetZSetItemVal(const std::string &item_key, double *score) {
    std::string str_score;
    leveldb::Status s = ldb->Get(leveldb::ReadOptions(), item_key, &str_score);
    if (s.IsNotFound()) {
        return 0;
    }
    if (!s.ok()) {
        log_error("%s", s.ToString().c_str());
        return STORAGE_ERR;
    }

    *score = *((double *) (str_score.data()));
    return 1;
}

//zscore
int SSDBImpl::zget(const Bytes &name, const Bytes &key, double *score) {
    *score = 0;

    ZSetMetaVal zv;
    std::string meta_key = encode_meta_key(name);
    int ret = GetZSetMetaVal(meta_key, zv);
    if (ret != 1) {
        return ret;
    }

    std::string str_score;
    std::string dbkey = encode_zset_key(name, key, zv.version);
    leveldb::Status s = ldb->Get(leveldb::ReadOptions(), dbkey, &str_score);
    if (s.IsNotFound()) {
        return 0;
    }
    if (!s.ok()) {
        log_error("%s", s.ToString().c_str());
        return STORAGE_ERR;
    }

    *score = *((double *) (str_score.data()));
    return 1;
}

ZIterator* SSDBImpl::zscan_internal(const Bytes &name, const Bytes &key_start,
                                    const Bytes &score_start, const Bytes &score_end,
                                    uint64_t limit, Iterator::Direction direction, uint16_t version,
                                    const leveldb::Snapshot *snapshot) {
    if (direction == Iterator::FORWARD) {
        std::string start, end;
        if (score_start.empty()) {
            start = encode_zscore_key(name, key_start, ZSET_SCORE_MIN, version);
        } else {
            start = encode_zscore_key(name, key_start, score_start.Double(), version);
        }
        if (score_end.empty()) {
            end = encode_zscore_key(name, "", ZSET_SCORE_MAX, version);
        } else {
            end = encode_zscore_key(name, "", score_end.Double(), version);
        }
        return new ZIterator(this->iterator(start, end, limit, snapshot), name, version);
    } else {
        std::string start, end;
        if (score_start.empty()) {
            start = encode_zscore_key(name, key_start, ZSET_SCORE_MAX, version);
        } else {
            if (key_start.empty()) {
                start = encode_zscore_key(name, "", score_start.Double(), version);
            } else {
                start = encode_zscore_key(name, key_start, score_start.Double(), version);
            }
        }
        if (score_end.empty()) {
            end = encode_zscore_key(name, "", ZSET_SCORE_MIN, version);
        } else {
            end = encode_zscore_key(name, "", score_end.Double(), version);
        }
        return new ZIterator(this->rev_iterator(start, end, limit, snapshot), name, version);
    }
}

int64_t SSDBImpl::zrank(const Bytes &name, const Bytes &key) {
    ZSetMetaVal zv;
    const leveldb::Snapshot* snapshot = nullptr;

    {
        RecordLock l(&mutex_record_, name.String());
        std::string meta_key = encode_meta_key(name);
        int res = GetZSetMetaVal(meta_key, zv);
        if (res != 1) {
            return -1;
        }

        snapshot = ldb->GetSnapshot();
    }

    SnapshotPtr spl(ldb, snapshot); //auto release

    bool found = false;
    auto it = std::unique_ptr<ZIterator>(this->zscan_internal(name, "", "", "", INT_MAX, Iterator::FORWARD, zv.version, snapshot));
    uint64_t ret = 0;
    while (true) {
        if (!it->next()) {
            break;
        }
        if (key == it->key) {
            found = true;
            break;
        }
        ret++;
    }
    return found ? ret : -1;
}

int64_t SSDBImpl::zrrank(const Bytes &name, const Bytes &key) {
    ZSetMetaVal zv;
    const leveldb::Snapshot* snapshot = nullptr;

    {
        RecordLock l(&mutex_record_, name.String());

        std::string meta_key = encode_meta_key(name);
        int res = GetZSetMetaVal(meta_key, zv);
        if (res != 1) {
            return -1;
        }

        snapshot = ldb->GetSnapshot();
    }

    SnapshotPtr spl(ldb, snapshot); //auto release
    bool found = false;
    auto it = std::unique_ptr<ZIterator>(this->zscan_internal(name, "", "", "", INT_MAX, Iterator::BACKWARD, zv.version, snapshot));
    uint64_t ret = 0;
    while (true) {
        if (!it->next()) {
            break;
        }
        if (key == it->key) {
            found = true;
            break;
        }
        ret++;
    }
    return found ? ret : -1;
}

int SSDBImpl::zrangeGeneric(const Bytes &name, const Bytes &begin, const Bytes &limit, std::vector<string> &key_score,
                            int reverse) {
    long long start, end;
    if (string2ll(begin.data(), (size_t)begin.size(), &start) == 0) {
        return INVALID_INT;
    } else if (start < LONG_MIN || start > LONG_MAX) {
        return INVALID_INT;
    }
    if (string2ll(limit.data(), (size_t)limit.size(), &end) == 0) {
        return INVALID_INT;
    } else if (end < LONG_MIN || end > LONG_MAX) {
        return INVALID_INT;
    }

    uint16_t version = 0;
    ZSetMetaVal zv;
    int llen = 0;
    const leveldb::Snapshot* snapshot = nullptr;
    ZIterator *it = NULL;

    {
        RecordLock l(&mutex_record_, name.String());
        std::string meta_key = encode_meta_key(name);
        int ret = GetZSetMetaVal(meta_key, zv);
        if (ret <= 0) {
            return ret;
        }

        llen = (int)zv.length;
        if (start < 0) start = llen+start;
        if (end < 0) end = llen+end;
        if (start < 0) start = 0;

        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen) {
            return 0;
        }
        if (end >= llen) end = llen-1;

        version = zv.version;
        snapshot = ldb->GetSnapshot();
    }

    if (reverse) {
        it = this->zscan_internal(name, "", "", "", end+1, Iterator::BACKWARD, version, snapshot);
    } else {
        it = this->zscan_internal(name, "", "",  "", end+1, Iterator::FORWARD, version, snapshot);
    }

    if (it != NULL) {
        it->skip(start);
        while(it->next()){
            key_score.push_back(it->key);
            key_score.push_back(str(it->score));
        }
        delete it;
        it = NULL;
    }

    if (snapshot != nullptr) {
        ldb->ReleaseSnapshot(snapshot);
    }

    return 1;
}

int SSDBImpl::zrange(const Bytes &name, const Bytes &begin, const Bytes &limit, std::vector<std::string> &key_score) {
    return zrangeGeneric(name, begin, limit, key_score, 0);
}

int SSDBImpl::zrrange(const Bytes &name, const Bytes &begin, const Bytes &limit, std::vector<std::string> &key_score) {
    return zrangeGeneric(name, begin, limit, key_score, 1);
}

/* Struct to hold a inclusive/exclusive range spec by score comparison. */
typedef struct {
    double min, max;
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Populate the rangespec according to the objects min and max. */
static int zslParseRange(const Bytes &min, const Bytes &max, zrangespec *spec) {
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */

    if (min[0] == '(') {
        spec->min = strtod(min.data()+1,&eptr);
        if (eptr[0] != '\0' || std::isnan(spec->min)) return NAN_SCORE;
        if (spec->min < ZSET_SCORE_MIN || spec->min > ZSET_SCORE_MAX) return ZSET_OVERFLOW;
        spec->minex = 1;
    } else {
        spec->min = strtod(min.data(),&eptr);
        if (eptr[0] != '\0' || std::isnan(spec->min)) return NAN_SCORE;
        if (spec->min < ZSET_SCORE_MIN || spec->min > ZSET_SCORE_MAX) return ZSET_OVERFLOW;
    }

    if (max[0] == '(') {
        spec->max = strtod(max.data()+1,&eptr);
        if (eptr[0] != '\0' || std::isnan(spec->max)) return NAN_SCORE;
        if (spec->max < ZSET_SCORE_MIN || spec->max > ZSET_SCORE_MAX) return ZSET_OVERFLOW;
        spec->maxex = 1;
    } else {
        spec->max = strtod(max.data(),&eptr);
        if (eptr[0] != '\0' || std::isnan(spec->max)) return NAN_SCORE;
        if (spec->max < ZSET_SCORE_MIN || spec->max > ZSET_SCORE_MAX) return ZSET_OVERFLOW;
    }

    return 1;
}

int zslValueGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

int zslValueLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

int SSDBImpl::genericZrangebyscore(const Bytes &name, const Bytes &start_score, const Bytes &end_score,
                                   std::vector<std::string> &key_score, int withscores, long offset, long limit, int reverse) {
    zrangespec range;
    std::string score_start;
    std::string score_end;
    int ret = 0;

    if (reverse){
        if ((ret = zslParseRange(end_score,start_score,&range)) < 0) {
            return ret;
        }
        double score = range.max + eps;
        score_start = str(score);
        score = range.min - eps;
        score_end = str(score);
    } else{
        if ((ret = zslParseRange(start_score,end_score,&range)) < 0) {
            return ret;
        }
        double score = range.min - eps;
        score_start = str(score);
        score = range.max + eps;
        score_end = str(score);
    }

    if (start_score == "-inf"){
        score_start = str(ZSET_SCORE_MIN);
    } else if(start_score == "+inf"){
        score_start = str(ZSET_SCORE_MAX);
    }
    if (end_score == "-inf"){
        score_end = str(ZSET_SCORE_MIN);
    } else if (end_score == "+inf"){
        score_end = str(ZSET_SCORE_MAX);
    }

    ZSetMetaVal zv;
    const leveldb::Snapshot* snapshot = nullptr;
    ZIterator *it = NULL;
    {
        RecordLock l(&mutex_record_, name.String());
        std::string meta_key = encode_meta_key(name);
        ret = GetZSetMetaVal(meta_key, zv);
        if (ret <= 0) {
            return ret;
        }
        snapshot = ldb->GetSnapshot();
    }

    if (reverse) {
        it = this->zscan_internal(name, "", score_start, score_end, -1, Iterator::BACKWARD, zv.version, snapshot);
        if (it == NULL) {
            if (snapshot != nullptr) {
                ldb->ReleaseSnapshot(snapshot);
            }
            return 1;
        }

        while (it->next()) {
            if (!zslValueLteMax(it->score,&range))
                continue;
            /* Check if score >= min. */
            if (!zslValueGteMin(it->score,&range))
                break;

            if (!offset){
                if (limit--){
                    key_score.push_back(it->key);
                    if (withscores){
                        key_score.push_back(str(it->score));
                    }
                } else
                    break;
            } else{
                offset--;
            }
        }
        delete it;
        it = NULL;
    } else {
        it = this->zscan_internal(name, "", score_start, score_end, -1, Iterator::FORWARD, zv.version, snapshot);
        if (it == NULL) {
            if (snapshot != nullptr) {
                ldb->ReleaseSnapshot(snapshot);
            }
            return 1;
        }

        while (it->next()) {
            if (!zslValueGteMin(it->score,&range))
                continue;
                /* Check if score <= max. */
            if (!zslValueLteMax(it->score,&range))
                break;

            if (!offset){
                if (limit--){
                    key_score.push_back(it->key);
                    if (withscores){
                        key_score.push_back(str(it->score));
                    }
                } else
                    break;
            } else{
                offset--;
            }
        }
        delete it;
        it = NULL;
    }

    if (snapshot != nullptr) {
        ldb->ReleaseSnapshot(snapshot);
    }

    return 1;
}

int SSDBImpl::zrangebyscore(const Bytes &name, const Bytes &start_score, const Bytes &end_score,
                            std::vector<std::string> &key_score, int withscores, long offset, long limit) {
    return genericZrangebyscore(name, start_score, end_score, key_score, withscores, offset, limit, 0);
}

int SSDBImpl::zrevrangebyscore(const Bytes &name, const Bytes &start_score, const Bytes &end_score,
                               std::vector<std::string> &key_score, int withscores, long offset, long limit) {
    return genericZrangebyscore(name, start_score, end_score, key_score, withscores, offset, limit, 1);
}

int64_t SSDBImpl::zremrangebyscore(const Bytes &name, const Bytes &score_start, const Bytes &score_end) {
    zrangespec range;
    int ret = 0;
    int64_t count = 0;
    std::string start_score;
    std::string end_score;

    if ((ret = zslParseRange(score_start,score_end,&range)) < 0) {
        return ret;
    }
    double score = range.min - eps;
    start_score = str(score);
    score = range.max + eps;
    end_score = str(score);

    if (score_start == "-inf"){
        start_score = str(ZSET_SCORE_MIN);
    } else if(score_start == "+inf"){
        start_score = str(ZSET_SCORE_MAX);
    }
    if (score_end == "-inf"){
        end_score = str(ZSET_SCORE_MIN);
    } else if (score_end == "+inf"){
        end_score = str(ZSET_SCORE_MAX);
    }

    RecordLock l(&mutex_record_, name.String());
    ZSetMetaVal zv;
    std::string meta_key = encode_meta_key(name);
    int res = GetZSetMetaVal(meta_key, zv);
    if (res <= 0) {
        return res;
    }

    leveldb::WriteBatch batch;
    const leveldb::Snapshot *snapshot = nullptr;
    auto it = std::unique_ptr<ZIterator>(this->zscan_internal(name, "", start_score, end_score, -1, Iterator::FORWARD, zv.version, snapshot));
    if (it == NULL){
        return 1;
    }

    while(it->next()){
        if (!zslValueGteMin(it->score,&range))
            continue;
        /* Check if score <= max. */
        if (!zslValueLteMax(it->score,&range))
            break;

        ret = zdel_one(this, batch, name, it->key, it->version);
        if(ret < 0){
            return ret;
        }
        count ++;
    }

    if (count > 0){
        ret = incr_zsize(this, batch, zv, name, count);
        if (ret < 0){
            return ret;
        }

        leveldb::Status s = ldb->Write(leveldb::WriteOptions(), &(batch));
        if (!s.ok()) {
            log_error("zdel error: %s", s.ToString().c_str());
            return STORAGE_ERR;
        }
    }

    return count;
}

ZIterator *SSDBImpl::zscan(const Bytes &name, const Bytes &key,
                           const Bytes &score_start, const Bytes &score_end, uint64_t limit) {
    uint16_t version = 0;
    ZSetMetaVal zv;
    std::string meta_key = encode_meta_key(name);
    int res = GetZSetMetaVal(meta_key, zv);
    if (res == 1) {
        version = zv.version;
    }

    std::string score;
    // if only key is specified, load its value
    if (!key.empty() && score_start.empty()) {
        double d_score = 0;
        this->zget(name, key, &d_score);
        score = str(d_score);

    } else {
        score = score_start.String();
    }
    return this->zscan_internal(name, key, score, score_end, limit, Iterator::FORWARD, version);
}


// returns the number of newly added items
static int zset_one(SSDBImpl *ssdb, leveldb::WriteBatch &batch, const Bytes &name, const Bytes &key, double score, uint16_t version) {

    string zkey = encode_zset_key(name, key, version);

    string buf;
    buf.append((char *) (&score), sizeof(double));

    string score_key = encode_zscore_key(name, key, score, version);

    batch.Put(zkey, buf);
    batch.Put(score_key, "");

    return 1;
}

static int zdel_one(SSDBImpl *ssdb, leveldb::WriteBatch &batch, const Bytes &name, const Bytes &key, uint16_t version) {
    double old_score = 0;
    std::string item_key = encode_zset_key(name, key, version);
    int ret = ssdb->GetZSetItemVal(item_key, &old_score);
    if (ret <= 0) {
        return ret;
    } else {
        //found
        std::string old_score_key = encode_zscore_key(name, key, old_score, version);
        std::string old_zset_key = encode_zset_key(name, key, version);

        batch.Delete(old_score_key);
        batch.Delete(old_zset_key);
    }

    return 1;
}

static int incr_zsize(SSDBImpl *ssdb, leveldb::WriteBatch &batch, const ZSetMetaVal &zv, const Bytes &name, int64_t incr) {
    std::string size_key = encode_meta_key(name);
    if (zv.del == KEY_DELETE_MASK) {
        if (incr > 0) {
            uint16_t version;
            if (zv.version == UINT16_MAX){
                version = 0;
            } else{
                version = (uint16_t) (zv.version + 1);
            }
            std::string size_val = encode_zset_meta_val((uint64_t) incr, version);
            batch.Put(size_key, size_val);
        } else {
            return -1;
        }
    } else if (zv.length == 0) {
        if (incr > 0) {
            std::string size_val = encode_zset_meta_val((uint64_t) incr);
            batch.Put(size_key, size_val);
        } else {
            return -1;
        }
    } else {
        uint64_t len = zv.length;
        if (incr > 0) {
            len += incr;
        } else if (incr < 0) {
            uint64_t u64 = static_cast<uint64_t>(-incr);
            if (len < u64) {
                return -1;
            }
            len = len - u64;
        }
        if (len == 0) {
            std::string del_key = encode_delete_key(name, zv.version);
            std::string meta_val = encode_zset_meta_val(zv.length, zv.version, KEY_DELETE_MASK);
            batch.Put(del_key, "");
            batch.Put(size_key, meta_val);
            ssdb->edel_one(batch, name); //del expire ET key
        } else {
            std::string size_val = encode_zset_meta_val(len, zv.version);
            batch.Put(size_key, size_val);
        }
    }
    return 0;
}

int64_t SSDBImpl::zfix(const Bytes &name) {
    RecordLock l(&mutex_record_, name.String());

    ZSetMetaVal zv;
    std::string meta_key = encode_meta_key(name);
    int ret = GetZSetMetaVal(meta_key, zv);
    if (ret != 1) {
        return ret;
    }

    std::string it_start, it_end;
    Iterator *it;
    leveldb::Status s;
    int64_t size = 0;
    int64_t old_size;

    it_start = encode_zscore_key(name, "", ZSET_SCORE_MIN, zv.version);
    it_end = encode_zscore_key(name, "", ZSET_SCORE_MAX, zv.version);
    it = this->iterator(it_start, it_end, UINT64_MAX);

    while (it->next()) {
        Bytes ks = it->key();
        //Bytes vs = it->val();
        //dump(ks.data(), ks.size(), "z.next");
        //dump(vs.data(), vs.size(), "z.next");
        if (ks.data()[0] != DataType::ZSCORE) {
            break;
        }
//        std::string name2, key, score;
//        if (decode_zscore_key(ks, &name2, &key, &score) == -1) {
//            size = -1;
//            break;
//        }
        ZScoreItemKey zk;
        if(zk.DecodeItemKey(ks) == -1){
            size = -1;
            break;
        }

        if (name != zk.key) {
            break;
        }
        size++;


        std::string buf = encode_zset_key(name, zk.field, zv.version);
        std::string score2;
        s = ldb->Get(leveldb::ReadOptions(), buf, &score2);
        if (!s.ok() && !s.IsNotFound()) {
            log_error("zget error: %s", s.ToString().c_str());
            size = -1;
            break;
        }

        double score = *((double *) (score2.data()));

        if (s.IsNotFound() || score != zk.score) {

            log_info("fix incorrect zset item, name: %s, key: %s, score: %s",
                     hexmem(name.data(), name.size()).c_str(),
                     hexmem(zk.field.data(), zk.field.size()).c_str(),
                     hexmem(score2.data(), score2.size()).c_str()
            );

            string score_buf;
            score_buf.append((char *) (&zk.score), sizeof(double));

            s = ldb->Put(leveldb::WriteOptions(), buf, score_buf);
            if (!s.ok()) {
                log_error("db error! %s", s.ToString().c_str());
                size = -1;
                break;
            }
        }
    }
    delete it;
    if (size == -1) {
        return -1;
    }

    old_size = this->zsize(name);
    if (old_size == -1) {
        return -1;
    }
    if (old_size != size) {
        log_info("fix zsize, name: %s, size: %"
                         PRId64
                         " => %"
                         PRId64,
                 hexmem(name.data(), name.size()).c_str(), old_size, size);

         zv.length = static_cast<uint64_t>(size);
        std::string size_val = encode_zset_meta_val(zv.length,zv.version);

        if (size == 0) {
//            s = ldb->Delete(leveldb::WriteOptions(), size_key);
        } else {
            s = ldb->Put(leveldb::WriteOptions(), meta_key, size_val);
        }
    }

    //////////////////////////////////////////

    it_start = encode_zset_key(name, "", zv.version); //bug here
    it_end = encode_zset_key(name, "", zv.version);
    it = this->iterator(it_start, "", UINT64_MAX);
    size = 0;
    while (it->next()) {
        Bytes ks = it->key();
        //Bytes vs = it->val();
        //dump(ks.data(), ks.size(), "z.next");
        //dump(vs.data(), vs.size(), "z.next");
        if (ks.data()[0] != DataType::ZSET) {
            break;
        }

        ZSetItemKey zk;
        zk.DecodeItemKey(ks);

        if (zk.DecodeItemKey(ks) == -1){
            size = -1;
            break;
        }

        if (name != zk.key) {
            break;
        }

        size++;

        Bytes score = it->val();
        double d_score = *((double *) score.String().data());
        std::string buf = encode_zscore_key(name, zk.field, d_score);
        std::string score2;
        s = ldb->Get(leveldb::ReadOptions(), buf, &score2);
        if (!s.ok() && !s.IsNotFound()) {
            log_error("zget error: %s", s.ToString().c_str());
            size = -1;
            break;
        }
        if (s.IsNotFound()) {
            log_info("fix incorrect zset score, name: %s, key: %s, score: %s",
                     hexmem(name.data(), name.size()).c_str(),
                     hexmem(zk.field.data(), zk.field.size()).c_str(),
                     hexmem(score.data(), score.size()).c_str()
            );
            s = ldb->Put(leveldb::WriteOptions(), buf, "");
            if (!s.ok()) {
                log_error("db error! %s", s.ToString().c_str());
                size = -1;
                break;
            }
        }
    }
    delete it;
    if (size == -1) {
        return -1;
    }

    old_size = this->zsize(name);
    if (old_size == -1) {
        return -1;
    }
    if (old_size != size) {
        log_info("fix zsize, name: %s, size: %"
                         PRId64
                         " => %"
                         PRId64,
                 hexmem(name.data(), name.size()).c_str(), old_size, size);
        zv.length = static_cast<uint64_t>(size);
        std::string size_val = encode_zset_meta_val(zv.length,zv.version);

        if (size == 0) {
//            s = ldb->Delete(leveldb::WriteOptions(), meta_key);
        } else {
            s = ldb->Put(leveldb::WriteOptions(), meta_key, size_val);
        }
    }

    //////////////////////////////////////////

    return size;
}
