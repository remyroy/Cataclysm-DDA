#include "mapbuffer.h"

#include <sstream>
#include <algorithm>
#include <exception>
#include <functional>
#include <set>
#include <utility>
#include <vector>

#include "cata_utility.h"
#include "coordinate_conversions.h"
#include "debug.h"
#include "filesystem.h"
#include "game.h"
#include "json.h"
#include "map.h"
#include "output.h"
#include "submap.h"
#include "translations.h"
#include "game_constants.h"

#define dbg(x) DebugLog((x),D_MAP) << __FILE__ << ":" << __LINE__ << ": "

mapbuffer MAPBUFFER;

mapbuffer::mapbuffer() = default;

mapbuffer::~mapbuffer()
{
    reset();
}

void mapbuffer::reset()
{
    for( auto &elem : submaps ) {
        delete elem.second;
    }
    submaps.clear();
}

bool mapbuffer::add_submap( const tripoint &p, submap *sm )
{
    if( submaps.count( p ) != 0 ) {
        return false;
    }

    submaps[p] = sm;

    return true;
}

bool mapbuffer::add_submap( int x, int y, int z, submap *sm )
{
    return add_submap( tripoint( x, y, z ), sm );
}

bool mapbuffer::add_submap( const tripoint &p, std::unique_ptr<submap> &sm )
{
    const bool result = add_submap( p, sm.get() );
    if( result ) {
        sm.release();
    }
    return result;
}

bool mapbuffer::add_submap( int x, int y, int z, std::unique_ptr<submap> &sm )
{
    return add_submap( tripoint( x, y, z ), sm );
}

void mapbuffer::remove_submap( tripoint addr )
{
    auto m_target = submaps.find( addr );
    if( m_target == submaps.end() ) {
        debugmsg( "Tried to remove non-existing submap %d,%d,%d", addr.x, addr.y, addr.z );
        return;
    }
    delete m_target->second;
    submaps.erase( m_target );
}

submap *mapbuffer::lookup_submap( int x, int y, int z )
{
    return lookup_submap( tripoint( x, y, z ) );
}

submap *mapbuffer::lookup_submap( const tripoint &p )
{
    dbg( D_INFO ) << "mapbuffer::lookup_submap( x[" << p.x << "], y[" << p.y << "], z[" << p.z << "])";

    const auto iter = submaps.find( p );
    if( iter == submaps.end() ) {
        try {
            return unserialize_submaps( p );
        } catch( const std::exception &err ) {
            debugmsg( "Failed to load submap (%d,%d,%d): %s", p.x, p.y, p.z, err.what() );
        }
        return nullptr;
    }

    return iter->second;
}

void mapbuffer::save( bool delete_after_save )
{
    std::stringstream map_directory;
    map_directory << g->get_world_base_save_path() << "/maps";
    assure_dir_exist( map_directory.str() );

    int num_saved_submaps = 0;
    int num_total_submaps = submaps.size();

    const tripoint map_origin = sm_to_omt_copy( g->m.get_abs_sub() );
    const bool map_has_zlevels = g != nullptr && g->m.has_zlevels();

    // A set of already-saved submaps, in global overmap coordinates.
    std::set<tripoint> saved_submaps;
    std::list<tripoint> submaps_to_delete;
    int next_report = 0;
    for( auto &elem : submaps ) {
        if( num_total_submaps > 100 && num_saved_submaps >= next_report ) {
            popup_nowait( _( "Please wait as the map saves [%d/%d]" ),
                          num_saved_submaps, num_total_submaps );
            next_report += std::max( 100, num_total_submaps / 20 );
        }

        // Whatever the coordinates of the current submap are,
        // we're saving a 2x2 quad of submaps at a time.
        // Submaps are generated in quads, so we know if we have one member of a quad,
        // we have the rest of it, if that assumption is broken we have REAL problems.
        const tripoint om_addr = sm_to_omt_copy( elem.first );
        if( saved_submaps.count( om_addr ) != 0 ) {
            // Already handled this one.
            continue;
        }
        saved_submaps.insert( om_addr );

        // A segment is a chunk of 32x32 submap quads.
        // We're breaking them into subdirectories so there aren't too many files per directory.
        // Might want to make a set for this one too so it's only checked once per save().
        std::stringstream dirname;
        tripoint segment_addr = omt_to_seg_copy( om_addr );
        dirname << map_directory.str() << "/" << segment_addr.x << "." <<
                segment_addr.y << "." << segment_addr.z;

        std::stringstream quad_path;
        quad_path << dirname.str() << "/" << om_addr.x << "." <<
                  om_addr.y << "." << om_addr.z << ".map";

        // delete_on_save deletes everything, otherwise delete submaps
        // outside the current map.
        const bool zlev_del = !map_has_zlevels && om_addr.z != g->get_levz();
        save_quad( dirname.str(), quad_path.str(), om_addr, submaps_to_delete,
                   delete_after_save || zlev_del ||
                   om_addr.x < map_origin.x || om_addr.y < map_origin.y ||
                   om_addr.x > map_origin.x + HALF_MAPSIZE ||
                   om_addr.y > map_origin.y + HALF_MAPSIZE );
        num_saved_submaps += 4;
    }
    for( auto &elem : submaps_to_delete ) {
        remove_submap( elem );
    }
}

void mapbuffer::save_quad( const std::string &dirname, const std::string &filename,
                           const tripoint &om_addr, std::list<tripoint> &submaps_to_delete,
                           bool delete_after_save )
{
    std::vector<point> offsets;
    std::vector<tripoint> submap_addrs;
    offsets.push_back( point_zero );
    offsets.push_back( point_south );
    offsets.push_back( point_east );
    offsets.push_back( point_south_east );

    bool all_uniform = true;
    for( auto &offsets_offset : offsets ) {
        tripoint submap_addr = omt_to_sm_copy( om_addr );
        submap_addr.x += offsets_offset.x;
        submap_addr.y += offsets_offset.y;
        submap_addrs.push_back( submap_addr );
        submap *sm = submaps[submap_addr];
        if( sm != nullptr && !sm->is_uniform ) {
            all_uniform = false;
        }
    }

    if( all_uniform ) {
        // Nothing to save - this quad will be regenerated faster than it would be re-read
        if( delete_after_save ) {
            for( auto &submap_addr : submap_addrs ) {
                if( submaps.count( submap_addr ) > 0 && submaps[submap_addr] != nullptr ) {
                    submaps_to_delete.push_back( submap_addr );
                }
            }
        }

        return;
    }

    // Don't create the directory if it would be empty
    assure_dir_exist( dirname );
    write_to_file( filename, [&]( std::ostream & fout ) {
        JsonOut jsout( fout );
        jsout.start_array();
        for( auto &submap_addr : submap_addrs ) {
            if( submaps.count( submap_addr ) == 0 ) {
                continue;
            }

            submap *sm = submaps[submap_addr];

            if( sm == nullptr ) {
                continue;
            }

            jsout.start_object();

            jsout.member( "version", savegame_version );
            jsout.member( "coordinates" );

            jsout.start_array();
            jsout.write( submap_addr.x );
            jsout.write( submap_addr.y );
            jsout.write( submap_addr.z );
            jsout.end_array();

            sm->store( jsout );

            jsout.end_object();

            if( delete_after_save ) {
                submaps_to_delete.push_back( submap_addr );
            }
        }

        jsout.end_array();
    } );
}

// We're reading in way too many entities here to mess around with creating sub-objects and
// seeking around in them, so we're using the json streaming API.
submap *mapbuffer::unserialize_submaps( const tripoint &p )
{
    // Map the tripoint to the submap quad that stores it.
    const tripoint om_addr = sm_to_omt_copy( p );
    const tripoint segment_addr = omt_to_seg_copy( om_addr );
    std::stringstream quad_path;
    quad_path << g->get_world_base_save_path() << "/maps/" <<
              segment_addr.x << "." << segment_addr.y << "." << segment_addr.z << "/" <<
              om_addr.x << "." << om_addr.y << "." << om_addr.z << ".map";

    using namespace std::placeholders;
    if( !read_from_file_optional_json( quad_path.str(),
                                       std::bind( &mapbuffer::deserialize, this, _1 ) ) ) {
        // If it doesn't exist, trigger generating it.
        return nullptr;
    }
    if( submaps.count( p ) == 0 ) {
        debugmsg( "file %s did not contain the expected submap %d,%d,%d",
                  quad_path.str(), p.x, p.y, p.z );
        return nullptr;
    }
    return submaps[ p ];
}

void mapbuffer::deserialize( JsonIn &jsin )
{
    jsin.start_array();
    while( !jsin.end_array() ) {
        std::unique_ptr<submap> sm = std::make_unique<submap>();
        tripoint submap_coordinates;
        jsin.start_object();
        bool rubpow_update = false;
        while( !jsin.end_object() ) {
            std::string submap_member_name = jsin.get_member_name();
            if( submap_member_name == "version" ) {
                if( jsin.get_int() < 22 ) {
                    rubpow_update = true;
                }
            } else if( submap_member_name == "coordinates" ) {
                jsin.start_array();
                int locx = jsin.get_int();
                int locy = jsin.get_int();
                int locz = jsin.get_int();
                jsin.end_array();
                submap_coordinates = tripoint( locx, locy, locz );
            } else {
                sm->load( jsin, submap_member_name, rubpow_update );
            }
        }

        if( !add_submap( submap_coordinates, sm ) ) {
            debugmsg( "submap %d,%d,%d was already loaded", submap_coordinates.x, submap_coordinates.y,
                      submap_coordinates.z );
        }
    }
}
