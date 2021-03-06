/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/database/databases/block_database.hpp>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <boost/filesystem.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/memory/memory.hpp>
#include <bitcoin/database/result/block_result.hpp>

namespace libbitcoin {
namespace database {

using namespace boost::filesystem;
using namespace bc::chain;

BC_CONSTEXPR size_t number_buckets = 600000;
BC_CONSTEXPR size_t header_size = slab_hash_table_header_size(number_buckets);
BC_CONSTEXPR size_t initial_map_file_size = header_size + minimum_slabs_size;

// Valid file offsets should never be zero.
const file_offset block_database::empty = 0;

// Record format:
// main:
//  [ header:80      ]
//  [ height:4       ]
//  [ number_txs:4   ]
// hashes:
//  [ [    ...     ] ]
//  [ [ tx_hash:32 ] ]
//  [ [    ...     ] ]

block_database::block_database(const path& map_filename,
    const path& index_filename, std::shared_ptr<shared_mutex> mutex)
  : lookup_file_(map_filename, mutex), 
    lookup_header_(lookup_file_, number_buckets),
    lookup_manager_(lookup_file_, header_size),
    lookup_map_(lookup_header_, lookup_manager_),
    index_file_(index_filename, mutex),
    index_manager_(index_file_, 0, sizeof(file_offset))
{
}

// Close does not call stop because there is no way to detect thread join.
block_database::~block_database()
{
    close();
}

// Create.
// ----------------------------------------------------------------------------

// Initialize files and open.
bool block_database::create()
{
    // Resize and create require a started file.
    if (!lookup_file_.open() ||
        !index_file_.open())
        return false;

    // These will throw if insufficient disk space.
    lookup_file_.resize(initial_map_file_size);
    index_file_.resize(minimum_records_size);

    if (!lookup_header_.create() ||
        !lookup_manager_.create() ||
        !index_manager_.create())
        return false;

    // Should not call start after create, already started.
    return
        lookup_header_.start() &&
        lookup_manager_.start() &&
        index_manager_.start();
}

// Startup and shutdown.
// ----------------------------------------------------------------------------

// Start files and primitives.
bool block_database::open()
{
    return
        lookup_file_.open() &&
        index_file_.open() &&
        lookup_header_.start() &&
        lookup_manager_.start() &&
        index_manager_.start();
}

// Close files.
bool block_database::close()
{
    return
        lookup_file_.close() &&
        index_file_.close();
}

// ----------------------------------------------------------------------------

bool block_database::exists(size_t height) const
{
    return height < index_manager_.count() && read_position(height) != empty;
}

block_result block_database::get(size_t height) const
{
    if (height >= index_manager_.count())
        return block_result(nullptr);

    const auto position = read_position(height);
    const auto memory = lookup_manager_.get(position);

    //*************************************************************************
    // HACK: back up into the slab to obtain the key (optimization).
    static const auto prefix_size = slab_row<hash_digest>::prefix_size;
    const auto buffer = REMAP_ADDRESS(memory);
    auto reader = make_unsafe_deserializer(buffer - prefix_size);
    //*************************************************************************

    return block_result(memory, std::move(reader.read_hash()));
}

block_result block_database::get(const hash_digest& hash) const
{
    const auto memory = lookup_map_.find(hash);
    return block_result(memory, hash);
}

void block_database::insert(const block& block, size_t height)
{
    BITCOIN_ASSERT(height <= max_uint32);
    const auto height32 = static_cast<uint32_t>(height);
    const auto tx_count = block.transactions().size();

    BITCOIN_ASSERT(tx_count <= max_uint32);
    const auto tx_count32 = static_cast<uint32_t>(tx_count);

    // Write block data.
    const auto write = [&](memory_ptr data)
    {
        auto serial = make_unsafe_serializer(REMAP_ADDRESS(data));
        serial.write_bytes(block.header().to_data());
        serial.write_4_bytes_little_endian(height32);
        serial.write_4_bytes_little_endian(tx_count32);

        for (const auto& tx: block.transactions())
            serial.write_hash(tx.hash());
    };

    const auto key = block.header().hash();
    const auto value_size = 80 + 4 + 4 + tx_count * hash_size;
    const auto position = lookup_map_.store(key, write, value_size);

    // Write position to index.
    write_position(position, height32);
}

bool block_database::gaps(heights& out_gaps) const
{
    const auto count = index_manager_.count();

    for (size_t height = 0; height < count; ++height)
        if (read_position(height) == empty)
            out_gaps.push_back(height);

    return true;
}

bool block_database::unlink(size_t from_height)
{
    if (index_manager_.count() > from_height)
    {
        index_manager_.set_count(from_height);
        return true;
    }

    return false;
}

void block_database::sync()
{
    lookup_manager_.sync();
    index_manager_.sync();
}

// This is necessary for parallel import, as gaps are created.
void block_database::zeroize(array_index first, array_index count)
{
    for (array_index index = first; index < (first + count); ++index)
    {
        const auto memory = index_manager_.get(index);
        auto serial = make_unsafe_serializer(REMAP_ADDRESS(memory));
        serial.write_8_bytes_little_endian(empty);
    }
}

// TODO: could relax the guards if only writing empty (headers).
void block_database::write_position(file_offset position, array_index height)
{
    BITCOIN_ASSERT(height < max_uint32);
    const auto new_count = height + 1;

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    mutex_.lock_upgrade();

    // Guard index_manager to prevent interim count increase.
    const auto initial_count = index_manager_.count();

    //+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    mutex_.unlock_upgrade_and_lock();

    // Guard write to prevent overwriting preceding height write.
    if (new_count > initial_count)
    {
        const auto create_count = new_count - initial_count;
        index_manager_.new_records(create_count);
        zeroize(initial_count, create_count - 1);
    }

    // Guard write to prevent subsequent zeroize from erasing.
    const auto memory = index_manager_.get(height);
    auto serial = make_unsafe_serializer(REMAP_ADDRESS(memory));
    serial.write_8_bytes_little_endian(position);

    mutex_.unlock();
    ///////////////////////////////////////////////////////////////////////////
}

file_offset block_database::read_position(array_index height) const
{
    const auto memory = index_manager_.get(height);
    const auto address = REMAP_ADDRESS(memory);
    return from_little_endian_unsafe<file_offset>(address);
}

// The index of the highest existing block, independent of gaps.
bool block_database::top(size_t& out_height) const
{
    const auto count = index_manager_.count();

    // Guard against no genesis block.
    if (count == 0)
        return false;

    out_height = count - 1;
    return true;
}

} // namespace database
} // namespace libbitcoin
