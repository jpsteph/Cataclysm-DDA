#include "avatar_action.h"

#include "action.h"
#include "avatar.h"
#include "creature.h"
#include "game.h"
#include "input.h"
#include "item.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "mapdata.h"
#include "map_iterator.h"
#include "messages.h"
#include "monster.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "player.h"
#include "translations.h"
#include "type_id.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_reference.h"

#define dbg(x) DebugLog((DebugLevel)(x),D_SDL) << __FILE__ << ":" << __LINE__ << ": "

static const trait_id trait_BURROW( "BURROW" );
static const trait_id trait_SHELL2( "SHELL2" );

static const efftype_id effect_amigara( "amigara" );
static const efftype_id effect_pet( "pet" );
static const efftype_id effect_relax_gas( "relax_gas" );
static const efftype_id effect_stunned( "stunned" );

bool avatar_action::move( avatar &you, map &m, int dx, int dy, int dz )
{
    if( ( !g->check_safe_mode_allowed() ) || you.has_active_mutation( trait_SHELL2 ) ) {
        if( you.has_active_mutation( trait_SHELL2 ) ) {
            add_msg( m_warning, _( "You can't move while in your shell.  Deactivate it to go mobile." ) );
        }
        return false;
    }

    tripoint dest_loc;
    if( dz == 0 && you.has_effect( effect_stunned ) ) {
        dest_loc.x = rng( you.posx() - 1, you.posx() + 1 );
        dest_loc.y = rng( you.posy() - 1, you.posy() + 1 );
        dest_loc.z = you.posz();
    } else {
        if( tile_iso && use_tiles && !you.has_destination() ) {
            rotate_direction_cw( dx, dy );
        }
        dest_loc.x = you.posx() + dx;
        dest_loc.y = you.posy() + dy;
        dest_loc.z = you.posz() + dz;
    }

    if( dest_loc == you.pos() ) {
        // Well that sure was easy
        return true;
    }

    if( m.has_flag( TFLAG_MINEABLE, dest_loc ) && g->mostseen == 0 &&
        get_option<bool>( "AUTO_FEATURES" ) && get_option<bool>( "AUTO_MINING" ) &&
        !m.veh_at( dest_loc ) && !you.is_underwater() && !you.has_effect( effect_stunned ) ) {
        if( you.weapon.has_flag( "DIG_TOOL" ) ) {
            if( you.weapon.type->can_use( "JACKHAMMER" ) && you.weapon.ammo_sufficient() ) {
                you.invoke_item( &you.weapon, "JACKHAMMER", dest_loc );
                you.defer_move( dest_loc ); // don't move into the tile until done mining
                return true;
            } else if( you.weapon.type->can_use( "PICKAXE" ) ) {
                you.invoke_item( &you.weapon, "PICKAXE", dest_loc );
                you.defer_move( dest_loc ); // don't move into the tile until done mining
                return true;
            }
        }
        if( you.has_trait( trait_BURROW ) ) {
            item burrowing_item( itype_id( "fake_burrowing" ) );
            you.invoke_item( &burrowing_item, "BURROW", dest_loc );
            you.defer_move( dest_loc ); // don't move into the tile until done mining
            return true;
        }
    }

    // by this point we're either walking, running, crouching, or attacking, so update the activity level to match
    if( you.get_movement_mode() == "walk" ) {
        you.increase_activity_level( LIGHT_EXERCISE );
    } else if( you.get_movement_mode() == "crouch" ) {
        you.increase_activity_level( MODERATE_EXERCISE );
    } else {
        you.increase_activity_level( ACTIVE_EXERCISE );
    }

    // If the player is *attempting to* move on the X axis, update facing direction of their sprite to match.
    const int new_dx = dest_loc.x - you.posx();
    if( new_dx > 0 ) {
        you.facing = FD_RIGHT;
    } else if( new_dx < 0 ) {
        you.facing = FD_LEFT;
    }

    if( dz == 0 && ramp_move( you, m, dest_loc ) ) {
        // TODO: Make it work nice with automove (if it doesn't do so already?)
        return false;
    }

    if( you.has_effect( effect_amigara ) ) {
        int curdist = INT_MAX;
        int newdist = INT_MAX;
        const tripoint minp = tripoint( 0, 0, you.posz() );
        const tripoint maxp = tripoint( MAPSIZE_X, MAPSIZE_Y, you.posz() );
        for( const tripoint &pt : m.points_in_rectangle( minp, maxp ) ) {
            if( m.ter( pt ) == t_fault ) {
                int dist = rl_dist( pt, you.pos() );
                if( dist < curdist ) {
                    curdist = dist;
                }
                dist = rl_dist( pt, dest_loc );
                if( dist < newdist ) {
                    newdist = dist;
                }
            }
        }
        if( newdist > curdist ) {
            add_msg( m_info, _( "You cannot pull yourself away from the faultline..." ) );
            return false;
        }
    }

    dbg( D_PEDANTIC_INFO ) << "game:plmove: From (" <<
                           you.posx() << "," << you.posy() << "," << you.posz() << ") to (" <<
                           dest_loc.x << "," << dest_loc.y << "," << dest_loc.z << ")";

    if( g->disable_robot( dest_loc ) ) {
        return false;
    }

    // Check if our movement is actually an attack on a monster or npc
    // Are we displacing a monster?

    bool attacking = false;
    if( g->critter_at( dest_loc ) ) {
        attacking = true;
    }

    if( !you.move_effects( attacking ) ) {
        you.moves -= 100;
        return false;
    }

    if( monster *const mon_ptr = g->critter_at<monster>( dest_loc, true ) ) {
        monster &critter = *mon_ptr;
        if( critter.friendly == 0 &&
            !critter.has_effect( effect_pet ) ) {
            if( you.has_destination() ) {
                add_msg( m_warning, _( "Monster in the way. Auto-move canceled." ) );
                add_msg( m_info, _( "Click directly on monster to attack." ) );
                you.clear_destination();
                return false;
            } else {
                // fighting is hard work!
                you.increase_activity_level( EXTRA_EXERCISE );
            }
            if( you.has_effect( effect_relax_gas ) ) {
                if( one_in( 8 ) ) {
                    add_msg( m_good, _( "Your willpower asserts itself, and so do you!" ) );
                } else {
                    you.moves -= rng( 2, 8 ) * 10;
                    add_msg( m_bad, _( "You're too pacified to strike anything..." ) );
                    return false;
                }
            }
            you.melee_attack( critter, true );
            if( critter.is_hallucination() ) {
                critter.die( &you );
            }
            g->draw_hit_mon( dest_loc, critter, critter.is_dead() );
            return false;
        } else if( critter.has_flag( MF_IMMOBILE ) ) {
            add_msg( m_info, _( "You can't displace your %s." ), critter.name() );
            return false;
        }
        // Successful displacing is handled (much) later
    }
    // If not a monster, maybe there's an NPC there
    if( npc *const np_ = g->critter_at<npc>( dest_loc ) ) {
        npc &np = *np_;
        if( you.has_destination() ) {
            add_msg( _( "NPC in the way, Auto-move canceled." ) );
            add_msg( m_info, _( "Click directly on NPC to attack." ) );
            you.clear_destination();
            return false;
        }

        if( !np.is_enemy() ) {
            g->npc_menu( np );
            return false;
        }

        you.melee_attack( np, true );
        // fighting is hard work!
        you.increase_activity_level( EXTRA_EXERCISE );
        np.make_angry();
        return false;
    }

    // GRAB: pre-action checking.
    int dpart = -1;
    const optional_vpart_position vp0 = m.veh_at( you.pos() );
    vehicle *const veh0 = veh_pointer_or_null( vp0 );
    const optional_vpart_position vp1 = m.veh_at( dest_loc );
    vehicle *const veh1 = veh_pointer_or_null( vp1 );

    bool veh_closed_door = false;
    bool outside_vehicle = ( veh0 == nullptr || veh0 != veh1 );
    if( veh1 != nullptr ) {
        dpart = veh1->next_part_to_open( vp1->part_index(), outside_vehicle );
        veh_closed_door = dpart >= 0 && !veh1->parts[dpart].open;
    }

    if( veh0 != nullptr && abs( veh0->velocity ) > 100 ) {
        if( veh1 == nullptr ) {
            if( query_yn( _( "Dive from moving vehicle?" ) ) ) {
                g->moving_vehicle_dismount( dest_loc );
            }
            return false;
        } else if( veh1 != veh0 ) {
            add_msg( m_info, _( "There is another vehicle in the way." ) );
            return false;
        } else if( !vp1.part_with_feature( "BOARDABLE", true ) ) {
            add_msg( m_info, _( "That part of the vehicle is currently unsafe." ) );
            return false;
        }
    }

    bool toSwimmable = m.has_flag( "SWIMMABLE", dest_loc );
    bool toDeepWater = m.has_flag( TFLAG_DEEP_WATER, dest_loc );
    bool fromSwimmable = m.has_flag( "SWIMMABLE", you.pos() );
    bool fromDeepWater = m.has_flag( TFLAG_DEEP_WATER, you.pos() );
    bool fromBoat = veh0 != nullptr && veh0->is_in_water();
    bool toBoat = veh1 != nullptr && veh1->is_in_water();

    if( toSwimmable && toDeepWater && !toBoat ) {  // Dive into water!
        // Requires confirmation if we were on dry land previously
        if( ( fromSwimmable && fromDeepWater && !fromBoat ) || query_yn( _( "Dive into the water?" ) ) ) {
            if( ( !fromDeepWater || fromBoat ) && you.swim_speed() < 500 ) {
                add_msg( _( "You start swimming." ) );
                add_msg( m_info, _( "%s to dive underwater." ),
                         press_x( ACTION_MOVE_DOWN ) );
            }
            g->plswim( dest_loc );
        }

        g->on_move_effects();
        return true;
    }

    //Wooden Fence Gate (or equivalently walkable doors):
    // open it if we are walking
    // vault over it if we are running
    if( m.passable_ter_furn( dest_loc )
        && you.get_movement_mode() == "walk"
        && m.open_door( dest_loc, !m.is_outside( you.pos() ) ) ) {
        you.moves -= 100;
        // if auto-move is on, continue moving next turn
        if( you.has_destination() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    if( g->walk_move( dest_loc ) ) {
        return true;
    }

    if( g->phasing_move( dest_loc ) ) {
        return true;
    }

    if( veh_closed_door ) {
        if( outside_vehicle ) {
            veh1->open_all_at( dpart );
        } else {
            veh1->open( dpart );
            add_msg( _( "You open the %1$s's %2$s." ), veh1->name,
                     veh1->part_info( dpart ).name() );
        }
        you.moves -= 100;
        // if auto-move is on, continue moving next turn
        if( you.has_destination() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    if( m.furn( dest_loc ) != f_safe_c && m.open_door( dest_loc, !m.is_outside( you.pos() ) ) ) {
        you.moves -= 100;
        // if auto-move is on, continue moving next turn
        if( you.has_destination() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    // Invalid move
    const bool waste_moves = you.is_blind() || you.has_effect( effect_stunned );
    if( waste_moves || dest_loc.z != you.posz() ) {
        add_msg( _( "You bump into the %s!" ), m.obstacle_name( dest_loc ) );
        // Only lose movement if we're blind
        if( waste_moves ) {
            you.moves -= 100;
        }
    } else if( m.ter( dest_loc ) == t_door_locked || m.ter( dest_loc ) == t_door_locked_peep ||
               m.ter( dest_loc ) == t_door_locked_alarm || m.ter( dest_loc ) == t_door_locked_interior ) {
        // Don't drain move points for learning something you could learn just by looking
        add_msg( _( "That door is locked!" ) );
    } else if( m.ter( dest_loc ) == t_door_bar_locked ) {
        add_msg( _( "You rattle the bars but the door is locked!" ) );
    }
    return false;
}

bool avatar_action::ramp_move( avatar &you, map &m, const tripoint &dest_loc )
{
    if( dest_loc.z != you.posz() ) {
        // No recursive ramp_moves
        return false;
    }

    // We're moving onto a tile with no support, check if it has a ramp below
    if( !m.has_floor_or_support( dest_loc ) ) {
        tripoint below( dest_loc.x, dest_loc.y, dest_loc.z - 1 );
        if( m.has_flag( TFLAG_RAMP, below ) ) {
            // But we're moving onto one from above
            const tripoint dp = dest_loc - you.pos();
            move( you, m, dp.x, dp.y, -1 );
            // No penalty for misaligned stairs here
            // Also cheaper than climbing up
            return true;
        }

        return false;
    }

    if( !m.has_flag( TFLAG_RAMP, you.pos() ) ||
        m.passable( dest_loc ) ) {
        return false;
    }

    // Try to find an aligned end of the ramp that will make our climb faster
    // Basically, finish walking on the stairs instead of pulling self up by hand
    bool aligned_ramps = false;
    for( const tripoint &pt : m.points_in_radius( you.pos(), 1 ) ) {
        if( rl_dist( pt, dest_loc ) < 2 && m.has_flag( "RAMP_END", pt ) ) {
            aligned_ramps = true;
            break;
        }
    }

    const tripoint above_u( you.posx(), you.posy(), you.posz() + 1 );
    if( m.has_floor_or_support( above_u ) ) {
        add_msg( m_warning, _( "You can't climb here - there's a ceiling above." ) );
        return false;
    }

    const tripoint dp = dest_loc - you.pos();
    const tripoint old_pos = you.pos();
    move( you, m, dp.x, dp.y, 1 );
    // We can't just take the result of the above function here
    if( you.pos() != old_pos ) {
        you.moves -= 50 + ( aligned_ramps ? 0 : 50 );
    }

    return true;
}

static float rate_critter( const Creature &c )
{
    const npc *np = dynamic_cast<const npc *>( &c );
    if( np != nullptr ) {
        return np->weapon_value( np->weapon );
    }

    const monster *m = dynamic_cast<const monster *>( &c );
    return m->type->difficulty;
}

void avatar_action::autoattack( avatar &you, map &m )
{
    int reach = you.weapon.reach_range( you );
    auto critters = you.get_hostile_creatures( reach );
    if( critters.empty() ) {
        add_msg( m_info, _( "No hostile creature in reach. Waiting a turn." ) );
        if( g->check_safe_mode_allowed() ) {
            you.pause();
        }
        return;
    }

    Creature &best = **std::max_element( critters.begin(), critters.end(),
    []( const Creature * l, const Creature * r ) {
        return rate_critter( *l ) > rate_critter( *r );
    } );

    const tripoint diff = best.pos() - you.pos();
    if( abs( diff.x ) <= 1 && abs( diff.y ) <= 1 && diff.z == 0 ) {
        move( you, m, diff.x, diff.y );
        return;
    }

    you.reach_attack( best.pos() );
}

// TODO: Move data/functions related to targeting out of game class
bool game::plfire_check( const targeting_data &args )
{
    // TODO: Make this check not needed
    if( args.relevant == nullptr ) {
        debugmsg( "Can't plfire_check a null" );
        return false;
    }

    if( u.has_effect( effect_relax_gas ) ) {
        if( one_in( 5 ) ) {
            add_msg( m_good, _( "Your eyes steel, and you raise your weapon!" ) );
        } else {
            u.moves -= rng( 2, 5 ) * 10;
            add_msg( m_bad, _( "You can't fire your weapon, it's too heavy..." ) );
            // break a possible loop when aiming
            if( u.activity ) {
                u.cancel_activity();
            }

            return false;
        }
    }

    item &weapon = *args.relevant;
    if( weapon.is_gunmod() ) {
        add_msg( m_info,
                 _( "The %s must be attached to a gun, it can not be fired separately." ),
                 weapon.tname() );
        return false;
    }

    auto gun = weapon.gun_current_mode();
    // check that a valid mode was returned and we are able to use it
    if( !( gun && u.can_use( *gun ) ) ) {
        add_msg( m_info, _( "You can no longer fire." ) );
        return false;
    }

    const optional_vpart_position vp = m.veh_at( u.pos() );
    if( vp && vp->vehicle().player_in_control( u ) && gun->is_two_handed( u ) ) {
        add_msg( m_info, _( "You need a free arm to drive!" ) );
        return false;
    }

    if( !weapon.is_gun() ) {
        // The weapon itself isn't a gun, this weapon is not fireable.
        return false;
    }

    if( gun->has_flag( "FIRE_TWOHAND" ) && ( !u.has_two_arms() ||
            u.worn_with_flag( "RESTRICT_HANDS" ) ) ) {
        add_msg( m_info, _( "You need two free hands to fire your %s." ), gun->tname() );
        return false;
    }

    // Skip certain checks if we are directly firing a vehicle turret
    if( args.mode != TARGET_MODE_TURRET_MANUAL ) {
        if( !gun->ammo_sufficient() && !gun->has_flag( "RELOAD_AND_SHOOT" ) ) {
            if( !gun->ammo_remaining() ) {
                add_msg( m_info, _( "You need to reload!" ) );
            } else {
                add_msg( m_info, _( "Your %s needs %i charges to fire!" ),
                         gun->tname(), gun->ammo_required() );
            }
            return false;
        }

        if( gun->get_gun_ups_drain() > 0 ) {
            const int ups_drain = gun->get_gun_ups_drain();
            const int adv_ups_drain = std::max( 1, ups_drain * 3 / 5 );

            if( !( u.has_charges( "UPS_off", ups_drain ) ||
                   u.has_charges( "adv_UPS_off", adv_ups_drain ) ||
                   ( u.has_active_bionic( bionic_id( "bio_ups" ) ) && u.power_level >= ups_drain ) ) ) {
                add_msg( m_info,
                         _( "You need a UPS with at least %d charges or an advanced UPS with at least %d charges to fire that!" ),
                         ups_drain, adv_ups_drain );
                return false;
            }
        }

        if( gun->has_flag( "MOUNTED_GUN" ) ) {
            const bool v_mountable = static_cast<bool>( m.veh_at( u.pos() ).part_with_feature( "MOUNTABLE",
                                     true ) );
            bool t_mountable = m.has_flag_ter_or_furn( "MOUNTABLE", u.pos() );
            if( !t_mountable && !v_mountable ) {
                add_msg( m_info,
                         _( "You must stand near acceptable terrain or furniture to use this weapon. A table, a mound of dirt, a broken window, etc." ) );
                return false;
            }
        }
    }

    return true;
}

bool game::plfire()
{
    targeting_data args = u.get_targeting_data();
    if( !args.relevant ) {
        // args missing a valid weapon, this shouldn't happen.
        debugmsg( "Player tried to fire a null weapon." );
        return false;
    }
    // If we were wielding this weapon when we started aiming, make sure we still are.
    bool lost_weapon = ( args.held && &u.weapon != args.relevant );
    bool failed_check = !plfire_check( args );
    if( lost_weapon || failed_check ) {
        u.cancel_activity();
        return false;
    }

    int reload_time = 0;
    gun_mode gun = args.relevant->gun_current_mode();

    // bows take more energy to fire than guns.
    u.weapon.is_gun() ? u.increase_activity_level( LIGHT_EXERCISE ) : u.increase_activity_level(
        MODERATE_EXERCISE );

    // TODO: move handling "RELOAD_AND_SHOOT" flagged guns to a separate function.
    if( gun->has_flag( "RELOAD_AND_SHOOT" ) ) {
        if( !gun->ammo_remaining() ) {
            item::reload_option opt = u.ammo_location &&
                                      gun->can_reload_with( u.ammo_location->typeId() ) ? item::reload_option( &u, args.relevant,
                                              args.relevant, u.ammo_location.clone() ) : u.select_ammo( *gun );
            if( !opt ) {
                // Menu canceled
                return false;
            }
            reload_time += opt.moves();
            if( !gun->reload( u, std::move( opt.ammo ), 1 ) ) {
                // Reload not allowed
                return false;
            }

            // Burn 2x the strength required to fire in stamina.
            u.mod_stat( "stamina", gun->get_min_str() * -2 );
            // At low stamina levels, firing starts getting slow.
            int sta_percent = ( 100 * u.stamina ) / u.get_stamina_max();
            reload_time += ( sta_percent < 25 ) ? ( ( 25 - sta_percent ) * 2 ) : 0;

            // Update targeting data to include ammo's range bonus
            args.range = gun.target->gun_range( &u );
            args.ammo = gun->ammo_data();
            u.set_targeting_data( args );

            refresh_all();
        }
    }

    temp_exit_fullscreen();
    m.draw( w_terrain, u.pos() );
    std::vector<tripoint> trajectory = target_handler().target_ui( u, args );

    if( trajectory.empty() ) {
        bool not_aiming = u.activity.id() != activity_id( "ACT_AIM" );
        if( not_aiming && gun->has_flag( "RELOAD_AND_SHOOT" ) ) {
            const auto previous_moves = u.moves;
            unload( *gun );
            // Give back time for unloading as essentially nothing has been done.
            // Note that reload_time has not been applied either.
            u.moves = previous_moves;
        }
        reenter_fullscreen();
        return false;
    }
    draw_ter(); // Recenter our view
    wrefresh( w_terrain );
    draw_panels();

    int shots = 0;

    u.moves -= reload_time;
    // TODO: add check for TRIGGERHAPPY
    if( args.pre_fire ) {
        args.pre_fire( shots );
    }
    shots = u.fire_gun( trajectory.back(), gun.qty, *gun );
    if( args.post_fire ) {
        args.post_fire( shots );
    }

    if( shots && args.power_cost ) {
        u.charge_power( -args.power_cost * shots );
    }
    reenter_fullscreen();
    return shots != 0;
}

bool game::plfire( item &weapon, int bp_cost )
{
    // TODO: bionic power cost of firing should be derived from a value of the relevant weapon.
    gun_mode gun = weapon.gun_current_mode();
    // gun can be null if the item is an unattached gunmod
    if( !gun ) {
        add_msg( m_info, _( "The %s can't be fired in its current state." ), weapon.tname() );
        return false;
    }

    targeting_data args = {
        TARGET_MODE_FIRE, &weapon, gun.target->gun_range( &u ),
        bp_cost, &u.weapon == &weapon, gun->ammo_data(),
        target_callback(), target_callback(),
        firing_callback(), firing_callback()
    };
    u.set_targeting_data( args );
    return plfire();
}
