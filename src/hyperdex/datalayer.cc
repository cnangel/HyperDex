// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// C
#include <cassert>

// POSIX
#include <sys/stat.h>
#include <sys/types.h>

// STL
#include <iomanip>
#include <set>
#include <sstream>
#include <tr1/functional>

// Google Log
#include <glog/logging.h>

// HyperDex
#include <hyperdex/datalayer.h>

typedef e::intrusive_ptr<hyperdex::region> region_ptr;
typedef std::map<e::buffer, std::pair<uint64_t, std::vector<e::buffer> > > region_map_t;

hyperdex :: datalayer :: datalayer()
    : m_shutdown(false)
    , m_flusher(std::tr1::bind(&datalayer::flush_loop, this))
    , m_lock()
    , m_regions()
{
    m_flusher.start();
}

hyperdex :: datalayer :: ~datalayer() throw ()
{
    if (!m_shutdown)
    {
        shutdown();
    }

    m_flusher.join();
}

std::set<hyperdex::regionid>
hyperdex :: datalayer :: regions()
{
    po6::threads::rwlock::rdhold hold(&m_lock);
    std::set<hyperdex::regionid> ret;

    for (std::map<regionid, region_ptr>::iterator i = m_regions.begin();
            i != m_regions.end(); ++i)
    {
        ret.insert(i->first);
    }

    return ret;
}

void
hyperdex :: datalayer :: create(const regionid& ri,
                                uint16_t numcolumns)
{
    po6::threads::rwlock::wrhold hold(&m_lock);
    std::map<regionid, region_ptr>::iterator i;
    i = m_regions.find(ri);

    if (i == m_regions.end())
    {
        std::ostringstream ostr;
        ostr << ri;

        if (mkdir(ostr.str().c_str(), S_IRWXU) < 0 && errno != EEXIST)
        {
            LOG(INFO) << "TRACE";
            throw po6::error(errno);
        }

        LOG(INFO) << "Creating " << ri << " with " << numcolumns << " columns "
                  << "in directory " << ostr.str();
        region_ptr reg;
        reg = new region(ri, ostr.str().c_str(), numcolumns);
        m_regions.insert(std::make_pair(ri, reg));
    }
    else
    {
        LOG(INFO) << ri << " already exists; cannot create region";
    }
}

void
hyperdex :: datalayer :: drop(const regionid& ri)
{
    po6::threads::rwlock::wrhold hold(&m_lock);
    std::map<regionid, region_ptr>::iterator i;
    i = m_regions.find(ri);

    if (i != m_regions.end())
    {
        LOG(INFO) << "Dropping " << ri;
        m_regions.erase(i);
    }
    else
    {
        LOG(INFO) << ri << " doesn't exist; cannot drop region";
    }
}

void
hyperdex :: datalayer :: shutdown()
{
    m_shutdown = true;
}

hyperdex :: result_t
hyperdex :: datalayer :: get(const regionid& ri,
                             const e::buffer& key,
                             std::vector<e::buffer>* value,
                             uint64_t* version)
{
    e::intrusive_ptr<hyperdex::region> r = get_region(ri);

    if (!r)
    {
        return INVALID;
    }

    return r->get(key, value, version);
}

hyperdex :: result_t
hyperdex :: datalayer :: put(const regionid& ri,
                             const e::buffer& key,
                             const std::vector<e::buffer>& value,
                             uint64_t version)
{
    e::intrusive_ptr<hyperdex::region> r = get_region(ri);

    if (!r)
    {
        return INVALID;
    }

    return r->put(key, value, version);
}

hyperdex :: result_t
hyperdex :: datalayer :: del(const regionid& ri,
                             const e::buffer& key)
{
    e::intrusive_ptr<hyperdex::region> r = get_region(ri);

    if (!r)
    {
        return INVALID;
    }

    return r->del(key);
}

e::intrusive_ptr<hyperdex::region>
hyperdex :: datalayer :: get_region(const regionid& ri)
{
    po6::threads::rwlock::rdhold hold(&m_lock);
    std::map<regionid, e::intrusive_ptr<region> >::iterator i;
    i = m_regions.find(ri);

    if (i == m_regions.end())
    {
        return e::intrusive_ptr<hyperdex::region>();
    }
    else
    {
        return i->second;
    }
}

void
hyperdex :: datalayer :: flush_loop()
{
    while (!m_shutdown)
    {
        std::set<e::intrusive_ptr<region> > to_flush;
        bool sleep = true;

        { // Hold the lock only in this scope.
            po6::threads::rwlock::rdhold hold(&m_lock);

            for (std::map<regionid, e::intrusive_ptr<region> >::iterator i = m_regions.begin();
                    i != m_regions.end(); ++i)
            {
                to_flush.insert(i->second);
            }
        }

        for (std::set<e::intrusive_ptr<region> >::iterator i = to_flush.begin();
                    i != to_flush.end(); ++i)
        {
            if ((*i)->flush() > 0)
            {
                sleep = false;
            }

            (*i)->async();
        }

        if (sleep)
        {
            timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000; // 100ms
            nanosleep(&ts, &ts); // If interrupted, it is no big deal.
        }
    }
}
