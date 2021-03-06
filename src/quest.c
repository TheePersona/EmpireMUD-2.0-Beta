/* ************************************************************************
*   File: quest.c                                         EmpireMUD 2.0b4 *
*  Usage: quest loading, saving, OLC, and processing                      *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2015                      *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.           *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */

#include <math.h>

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "db.h"
#include "comm.h"
#include "olc.h"
#include "skills.h"
#include "handler.h"
#include "dg_scripts.h"
#include "vnums.h"

/**
* Contents:
*   Helpers
*   Lookup Handlers
*   Quest Handlers
*   Utilities
*   Database
*   OLC Handlers
*   Displays
*   Edit Modules
*/

// local data
const char *default_quest_name = "Unnamed Quest";
const char *default_quest_description = "This quest has no description.\r\n";
const char *default_quest_complete_msg = "You have completed the quest.\r\n";

// external consts
extern const char *action_bits[];
extern const char *quest_flags[];
extern const char *quest_giver_types[];
extern const char *quest_reward_types[];
extern const char *quest_tracker_types[];
extern const char *olc_type_bits[NUM_OLC_TYPES+1];

// external funcs
extern int count_owned_buildings(empire_data *emp, bld_vnum vnum);
extern int count_owned_vehicles(empire_data *emp, any_vnum vnum);
void count_quest_tasks(struct player_quest *pq, int *complete, int *total);
void drop_quest(char_data *ch, struct player_quest *pq);
extern struct instance_data *find_instance_by_room(room_data *room, bool check_homeroom);
extern struct instance_data *get_instance_by_id(any_vnum instance_id);
void get_script_display(struct trig_proto_list *list, char *save_buffer);

// local protos
void add_quest_lookup(struct quest_lookup **list, quest_data *quest);
void add_to_quest_temp_list(struct quest_temp_list **list, quest_data *quest, struct instance_data *instance);
bool char_meets_prereqs(char_data *ch, quest_data *quest, struct instance_data *instance);
struct quest_task *copy_quest_tasks(struct quest_task *from);
bool find_quest_giver_in_list(struct quest_giver *list, int type, any_vnum vnum);
void free_player_quests(struct player_quest *list);
void free_quest_givers(struct quest_giver *list);
void free_quest_tasks(struct quest_task *list);
void free_quest_temp_list(struct quest_temp_list *list);
struct player_completed_quest *has_completed_quest(char_data *ch, any_vnum quest);
struct player_quest *is_on_quest(char_data *ch, any_vnum quest);
bool remove_quest_lookup(struct quest_lookup **list, quest_data *quest);
void update_mob_quest_lookups(mob_vnum vnum);
void update_obj_quest_lookups(obj_vnum vnum);


 //////////////////////////////////////////////////////////////////////////////
//// HELPERS /////////////////////////////////////////////////////////////////

/**
* Searches for a quest giver at a particular location. This does NOT guarantee
* the quest is complete or completable.
*
* @param char_data *ch The person looking.
* @param room_data *loc The location to check.
* @param quest_data *quest The quest to check.
* @param empire_data **giver_emp Somewhere to store the loyalty of the quest-giver.
* @return bool TRUE if ch can turn it in there; FALASE if not.
*/
bool can_turn_in_quest_at(char_data *ch, room_data *loc, quest_data *quest, empire_data **giver_emp) {
	struct quest_giver *giver;
	char_data *mob;
	obj_data *obj;
	
	*giver_emp = NULL;
	
	LL_FOREACH(QUEST_ENDS_AT(quest), giver) {
		// QG_x: find quest giver here
		switch (giver->type) {
			case QG_BUILDING: {
				if (GET_BUILDING(loc) && GET_BLD_VNUM(GET_BUILDING(loc)) == giver->vnum) {
					*giver_emp = ROOM_OWNER(loc);
					return TRUE;
				}
				break;
			}
			case QG_MOBILE: {
				LL_FOREACH2(ROOM_PEOPLE(IN_ROOM(ch)), mob, next_in_room) {
					if (IS_NPC(mob) && GET_MOB_VNUM(mob) == giver->vnum && CAN_SEE(ch, mob)) {
						*giver_emp = GET_LOYALTY(mob);
						return TRUE;
					}
				}
				break;
			}
			case QG_OBJECT: {
				LL_FOREACH2(ch->carrying, obj, next_content) {
					if (GET_OBJ_VNUM(obj) == giver->vnum && CAN_SEE_OBJ(ch, obj)) {
						*giver_emp = GET_LOYALTY(ch);
						return TRUE;
					}
				}
				LL_FOREACH2(ROOM_CONTENTS(loc), obj, next_content) {
					if (GET_OBJ_VNUM(obj) == giver->vnum && CAN_SEE_OBJ(ch, obj)) {
						*giver_emp = CAN_WEAR(obj, ITEM_WEAR_TAKE) ? GET_LOYALTY(ch) : ROOM_OWNER(loc);
						return TRUE;
					}
				}
			}
			case QG_ROOM_TEMPLATE: {
				if (GET_ROOM_TEMPLATE(loc) && GET_RMT_VNUM(GET_ROOM_TEMPLATE(loc)) == giver->vnum) {
					*giver_emp = ROOM_OWNER(loc);
					return TRUE;
				}
				break;
			}
			// case QG_TRIGGER: never local
		}
	}
	
	// nope
	return FALSE;
}


/**
* Copies progress from one list to another list, if they have any common
* entries. This is useful is tasks are changed on a quest, but players are
* already on the quest.
*
* @param struct quest_task *to_list List to copy progress TO.
* @param struct quest_task *from_list List to copy progress FROM.
*/
void copy_quest_progress(struct quest_task *to_list, struct quest_task *from_list) {
	struct quest_task *to_iter, *from_iter;
	
	LL_FOREACH(to_list, to_iter) {
		LL_FOREACH(from_list, from_iter) {
			if (to_iter->type != from_iter->type) {
				continue;
			}
			if (to_iter->vnum != from_iter->vnum) {
				continue;
			}
			if (to_iter->misc != from_iter->misc) {
				continue;
			}
			
			// seems to be the same
			to_iter->current = MAX(to_iter->current, from_iter->current);
		}
	}
}


/**
* Number of buildings owned by an empire.
*
* @param empire_data *emp Any empire.
* @param bld_vnum vnum The building to search for.
* @return int The number of completed buildings with that vnum, owned by emp.
*/
int count_owned_buildings(empire_data *emp, bld_vnum vnum) {
	struct empire_territory_data *ter;
	int count = 0;	// ah ah ah
	
	if (!emp || vnum == NOTHING) {
		return count;
	}
	
	LL_FOREACH(EMPIRE_TERRITORY_LIST(emp), ter) {
		if (!IS_COMPLETE(ter->room) || !GET_BUILDING(ter->room)) {
			continue;
		}
		if (GET_BLD_VNUM(GET_BUILDING(ter->room)) != vnum) {
			continue;
		}
		
		// found
		++count;
	}
	
	return count;
}


/**
* Number of vehicles owned by an empire.
*
* @param empire_data *emp Any empire.
* @param any_vnum vnum The vehicle to search for.
* @return int The number of completed vehicles with that vnum, owned by emp.
*/
int count_owned_vehicles(empire_data *emp, any_vnum vnum) {
	vehicle_data *veh;
	int count = 0;
	
	if (!emp || vnum == NOTHING) {
		return count;
	}
	
	LL_FOREACH(vehicle_list, veh) {
		if (!VEH_IS_COMPLETE(veh) || VEH_OWNER(veh) != emp) {
			continue;
		}
		if (VEH_VNUM(veh) != vnum) {
			continue;
		}
		
		// found
		++count;
	}
	
	return count;
}


/**
* Counts how many components a player has available for a quest.
*
* @param char_data *ch The player.
* @param int type CMP_ type
* @param bitvector_t flags CMPF_ flags to match
* @param bool skip_keep If TRUE, skips items marked (keep) because they can't be taken.
* @return int The number of matching component items.
*/
int count_quest_components(char_data *ch, int type, bitvector_t flags, bool skip_keep) {
	obj_data *obj;
	int count = 0;
	
	LL_FOREACH2(ch->carrying, obj, next_content) {
		if (GET_OBJ_CMP_TYPE(obj) != type) {
			continue;
		}
		if ((GET_OBJ_CMP_FLAGS(obj) & flags) != flags) {
			continue;
		}
		if (skip_keep && OBJ_FLAGGED(obj, OBJ_KEEP)) {
			continue;
		}
		
		++count;
	}
	
	return count;
}


/**
* Counts how many items a player has available for a quest.
*
* @param char_data *ch The player.
* @param obj_vnum vnum The vnum of the item to look for.
* @param bool skip_keep If TRUE, skips items marked (keep) because they can't be taken.
* @return int The number of matching items.
*/
int count_quest_objects(char_data *ch, obj_vnum vnum, bool skip_keep) {
	obj_data *obj;
	int count = 0;
	
	LL_FOREACH2(ch->carrying, obj, next_content) {
		if (GET_OBJ_VNUM(obj) != vnum) {
			continue;
		}
		if (skip_keep && OBJ_FLAGGED(obj, OBJ_KEEP)) {
			continue;
		}
		
		++count;
	}
	
	return count;
}


/**
* Ends all quests that are marked QST_EXPIRES_AFTER_INSTANCE.
*
* @param struct instance_data *inst The instance to check quests for.
*/
void expire_instance_quests(struct instance_data *inst) {
	struct player_quest *pq, *next_pq;
	descriptor_data *desc;
	quest_data *quest;
	char_data *ch;
	
	LL_FOREACH(descriptor_list, desc) {
		if (STATE(desc) != CON_PLAYING || !(ch = desc->character) || IS_NPC(ch)) {
			continue;
		}
		
		LL_FOREACH_SAFE(GET_QUESTS(ch), pq, next_pq) {
			if (pq->instance_id != inst->id || pq->adventure != GET_ADV_VNUM(inst->adventure)) {
				continue;
			}
			if (!(quest = quest_proto(pq->vnum))) {
				continue;
			}
			if (!QUEST_FLAGGED(quest, QST_EXPIRES_AFTER_INSTANCE)) {
				continue;
			}
			
			msg_to_char(ch, "You fail %s because the adventure instance ended.\r\n", QUEST_NAME(quest));
			drop_quest(ch, pq);
		}
	}
}


/**
* Quick way to turn a vnum into a name, safely.
*
* @param any_vnum vnum The quest vnum to look up.
* @return char* A name for the vnum, or "UNKNOWN".
*/
char *get_quest_name_by_proto(any_vnum vnum) {
	quest_data *proto = quest_proto(vnum);
	return proto ? QUEST_NAME(proto) : "UNKNOWN";
}


/**
* Gets standard string display like "4x lumber" for a quest giver (starts/ends
* at).
*
* @param struct quest_giver *giver The quest giver to show.
* @param bool show_vnums If TRUE, adds [1234] at the start of the string.
* @return char* The string display.
*/
char *quest_giver_string(struct quest_giver *giver, bool show_vnums) {
	static char output[256];
	char vnum[256];
	
	*output = '\0';
	if (!giver) {
		return output;
	}
	
	if (show_vnums) {
		snprintf(vnum, sizeof(vnum), "%s [%d] ", quest_giver_types[giver->type], giver->vnum);
	}
	else {
		*vnum = '\0';
	}
	
	// QG_x
	switch (giver->type) {
		case QG_BUILDING: {
			bld_data *bld = building_proto(giver->vnum);
			snprintf(output, sizeof(output), "%s%s", vnum, bld ? GET_BLD_NAME(bld) : "UNKNOWN");
			break;
		}
		case QG_MOBILE: {
			snprintf(output, sizeof(output), "%s%s", vnum, skip_filler(get_mob_name_by_proto(giver->vnum)));
			break;
		}
		case QG_OBJECT: {
			snprintf(output, sizeof(output), "%s%s", vnum, skip_filler(get_obj_name_by_proto(giver->vnum)));
			break;
		}
		case QG_ROOM_TEMPLATE: {
			room_template *rmt = room_template_proto(giver->vnum);
			snprintf(output, sizeof(output), "%s%s", vnum, rmt ? skip_filler(GET_RMT_TITLE(rmt)) : "UNKNOWN");
			break;
		}
		case QG_TRIGGER: {
			trig_data *trig = real_trigger(giver->vnum);
			snprintf(output, sizeof(output), "%s%s", vnum, trig ? skip_filler(GET_TRIG_NAME(trig)) : "UNKNOWN");
			break;
		}
		default: {
			snprintf(output, sizeof(output), "%d %sUNKNOWN", giver->type, vnum);
			break;
		}
	}
	
	return output;
}


/**
* Gets standard string display like "4x lumber" for a quest reward.
*
* @param struct quest_reward *reward The reward to show.
* @param bool show_vnums If TRUE, adds [1234] at the start of the string.
* @return char* The string display.
*/
char *quest_reward_string(struct quest_reward *reward, bool show_vnums) {
	static char output[256];
	char vnum[256];
	
	*output = '\0';
	if (!reward) {
		return output;
	}
	
	if (show_vnums) {
		snprintf(vnum, sizeof(vnum), "[%d] ", reward->vnum);
	}
	else {
		*vnum = '\0';
	}
	
	// QR_x
	switch (reward->type) {
		case QR_BONUS_EXP: {
			// has no vnum
			snprintf(output, sizeof(output), "%d bonus exp", reward->amount);
			break;
		}
		case QR_COINS: {
			// vnum not relevant
			snprintf(output, sizeof(output), "%d %s coin%s", reward->amount, reward->vnum == OTHER_COIN ? "misc" : "empire", PLURAL(reward->amount));
			break;
		}
		case QR_OBJECT: {
			snprintf(output, sizeof(output), "%s%dx %s", vnum, reward->amount, skip_filler(get_obj_name_by_proto(reward->vnum)));
			break;
		}
		case QR_SET_SKILL: {
			snprintf(output, sizeof(output), "%s%d %s", vnum, reward->amount, get_skill_name_by_vnum(reward->vnum));
			break;
		}
		case QR_SKILL_EXP: {
			snprintf(output, sizeof(output), "%s%d%% %s", vnum, reward->amount, get_skill_name_by_vnum(reward->vnum));
			break;
		}
		case QR_SKILL_LEVELS: {
			snprintf(output, sizeof(output), "%s%dx %s", vnum, reward->amount, get_skill_name_by_vnum(reward->vnum));
			break;
		}
		case QR_QUEST_CHAIN: {
			snprintf(output, sizeof(output), "%s%s", vnum, get_quest_name_by_proto(reward->vnum));
			break;
		}
		default: {
			snprintf(output, sizeof(output), "%s%dx UNKNOWN", vnum, reward->amount);
			break;
		}
	}
	
	return output;
}


/**
* Gets standard string display like "4x lumber" for a quest task.
*
* @param struct quest_task *task The task to show.
* @param bool show_vnums If TRUE, adds [1234] at the start of the string.
* @return char* The string display.
*/
char *quest_task_string(struct quest_task *task, bool show_vnums) {
	char vnum[256], lbuf[256];
	static char output[256];
	
	*output = '\0';
	if (!task) {
		return output;
	}
	
	if (show_vnums) {
		snprintf(vnum, sizeof(vnum), "[%d] ", task->vnum);
	}
	else {
		*vnum = '\0';
	}
	
	// QT_x
	switch (task->type) {
		case QT_COMPLETED_QUEST: {
			snprintf(output, sizeof(output), "Complete quest: %s%s", vnum, get_quest_name_by_proto(task->vnum));
			break;
		}
		case QT_GET_COMPONENT: {
			snprintf(output, sizeof(output), "Get component%s: %dx %s%s", PLURAL(task->needed), task->needed, vnum, component_string(task->vnum, task->misc));
			break;
		}
		case QT_GET_OBJECT: {
			snprintf(output, sizeof(output), "Get object%s: %dx %s%s", PLURAL(task->needed), task->needed, vnum, get_obj_name_by_proto(task->vnum));
			break;
		}
		case QT_KILL_MOB: {
			snprintf(output, sizeof(output), "Kill %dx mob%s: %s%s", task->needed, PLURAL(task->needed), vnum, get_mob_name_by_proto(task->vnum));
			break;
		}
		case QT_KILL_MOB_FLAGGED: {
			sprintbit(task->misc, action_bits, lbuf, TRUE);
			// does not show vnum
			snprintf(output, sizeof(output), "Kill %dx mob%s flagged: %s", task->needed, PLURAL(task->needed), lbuf);
			break;
		}
		case QT_NOT_COMPLETED_QUEST: {
			snprintf(output, sizeof(output), "Not completed quest %s%s", vnum, get_quest_name_by_proto(task->vnum));
			break;
		}
		case QT_NOT_ON_QUEST: {
			snprintf(output, sizeof(output), "Not on quest %s%s", vnum, get_quest_name_by_proto(task->vnum));
			break;
		}
		case QT_OWN_BUILDING: {
			bld_data *bld = building_proto(task->vnum);
			snprintf(output, sizeof(output), "Own %dx building%s: %s%s", task->needed, PLURAL(task->needed), vnum, bld ? GET_BLD_NAME(bld) : "UNKNOWN");
			break;
		}
		case QT_OWN_VEHICLE: {
			snprintf(output, sizeof(output), "Own %dx vehicle%s: %s%s", task->needed, PLURAL(task->needed), vnum, get_vehicle_name_by_proto(task->vnum));
			break;
		}
		case QT_SKILL_LEVEL_OVER: {
			snprintf(output, sizeof(output), "%s%s at least %d", vnum, get_skill_name_by_vnum(task->vnum), task->needed);
			break;
		}
		case QT_SKILL_LEVEL_UNDER: {
			snprintf(output, sizeof(output), "%s%s not over %d", vnum, get_skill_name_by_vnum(task->vnum), task->needed);
			break;
		}
		case QT_TRIGGERED: {
			strcpy(output, "Scripted condition");
			break;
		}
		case QT_VISIT_BUILDING: {
			bld_data *bld = building_proto(task->vnum);
			snprintf(output, sizeof(output), "Visit building: %s%s", vnum, bld ? GET_BLD_NAME(bld) : "UNKNOWN");
			break;
		}
		case QT_VISIT_ROOM_TEMPLATE: {
			room_template *rmt = room_template_proto(task->vnum);
			snprintf(output, sizeof(output), "Visit location: %s%s", vnum, rmt ? GET_RMT_TITLE(rmt) : "UNKNOWN");
			break;
		}
		case QT_VISIT_SECTOR: {
			sector_data *sect = sector_proto(task->vnum);
			snprintf(output, sizeof(output), "Visit terrain: %s%s", vnum, sect ? GET_SECT_NAME(sect) : "UNKNOWN");
			break;
		}
		case QT_HAVE_ABILITY: {
			snprintf(output, sizeof(output), "Have ability: %s%s", vnum, get_ability_name_by_vnum(task->vnum));
			break;
		}
		default: {
			sprintf(buf, "Unknown condition");
			break;
		}
	}
	
	return output;
}


/**
* Redetects counts for any quest task type that CAN be redetected.
* (Some, like mob kills, cannot).
*
* @param char_data *ch The player.
* @param struct player_quest *pq The player's quest to refresh.
*/
void refresh_one_quest_tracker(char_data *ch, struct player_quest *pq) {
	quest_data *quest = quest_proto(pq->vnum);
	struct quest_task *task;
	
	LL_FOREACH(pq->tracker, task) {
		// QT_x: refreshable types only
		switch (task->type) {
			case QT_COMPLETED_QUEST: {
				task->current = has_completed_quest(ch, task->vnum) ? task->needed : 0;
				break;
			}
			case QT_GET_COMPONENT: {
				task->current = count_quest_components(ch, task->vnum, task->misc, QUEST_FLAGGED(quest, QST_EXTRACT_TASK_OBJECTS));
				break;
			}
			case QT_GET_OBJECT: {
				task->current = count_quest_objects(ch, task->vnum, QUEST_FLAGGED(quest, QST_EXTRACT_TASK_OBJECTS));
				break;
			}
			case QT_NOT_COMPLETED_QUEST: {
				task->current = has_completed_quest(ch, task->vnum) ? 0 : task->needed;
				break;
			}
			case QT_NOT_ON_QUEST: {
				task->current = is_on_quest(ch, task->vnum) ? 0 : task->needed;
				break;
			}
			case QT_OWN_BUILDING: {
				task->current = count_owned_buildings(GET_LOYALTY(ch), task->vnum);
				break;
			}
			case QT_OWN_VEHICLE: {
				task->current = count_owned_vehicles(GET_LOYALTY(ch), task->vnum);
				break;
			}
			case QT_SKILL_LEVEL_OVER: {
				if (get_skill_level(ch, task->vnum) >= task->needed) {
					task->current = task->needed;	// full
				}
				else {
					task->current = 0;
				}
				break;
			}
			case QT_SKILL_LEVEL_UNDER: {
				if (get_skill_level(ch, task->vnum) <= task->needed) {
					task->current = MAX(0, task->needed);	// full
				}
				else {
					task->current = -1;	// must set below 0 because 0 is a valid "needed"
				}
				break;
			}
			case QT_VISIT_BUILDING: {
				if (GET_BUILDING(IN_ROOM(ch)) && GET_BLD_VNUM(GET_BUILDING(IN_ROOM(ch))) == task->vnum) {
					task->current = task->needed;	// full
				}
				// else can't detect this
				break;
			}
			case QT_VISIT_ROOM_TEMPLATE: {
				if (GET_ROOM_TEMPLATE(IN_ROOM(ch)) && GET_RMT_VNUM(GET_ROOM_TEMPLATE(IN_ROOM(ch))) == task->vnum) {
					task->current = task->needed;	// full
				}
				// else can't detect this
				break;
			}
			case QT_VISIT_SECTOR: {
				if (GET_SECT_VNUM(SECT(IN_ROOM(ch))) == task->vnum) {
					task->current = task->needed;	// full
				}
				// else can't detect this
				break;
			}
			case QT_HAVE_ABILITY: {
				task->current = has_ability(ch, task->vnum) ? task->needed : 0;	// full
				break;
			}
		}
	}
}


/**
* Makes sure all of a player's quest objectives are current.
*
* @param char_data *ch The player to check.
*/
void refresh_all_quests(char_data *ch) {
	struct player_quest *pq, *next_pq;
	struct instance_data *inst;
	struct quest_task *old;
	quest_data *quest;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH_SAFE(GET_QUESTS(ch), pq, next_pq) {
		// remove entirely
		if (!(quest = quest_proto(pq->vnum)) || (QUEST_FLAGGED(quest, QST_IN_DEVELOPMENT) && !IS_IMMORTAL(ch))) {
			drop_quest(ch, pq);
			continue;
		}
		// check instance expiry
		if (QUEST_FLAGGED(quest, QST_EXPIRES_AFTER_INSTANCE) && (!(inst = get_instance_by_id(pq->instance_id)) || GET_ADV_VNUM(inst->adventure) != pq->adventure)) {
			drop_quest(ch, pq);
			continue;
		}
		
		// reload objectives
		if (pq->version < QUEST_VERSION(quest)) {
			old = pq->tracker;
			pq->tracker = copy_quest_tasks(QUEST_TASKS(quest));
			copy_quest_progress(pq->tracker, old);
			free_quest_tasks(old);
		}
		
		// check tracker tasks now
		refresh_one_quest_tracker(ch, pq);
	}
}


/**
* Checks a character and removes all items for quests they're not on.
*
* Does not affect immortals.
*
* @param char_data *ch The player.
*/
void remove_quest_items(char_data *ch) {
	obj_data *obj, *next_obj;
	int iter;
	
	if (IS_NPC(ch) || IS_IMMORTAL(ch)) {
		return;
	}
	
	for (iter = 0; iter < NUM_WEARS; ++iter) {
		if ((obj = GET_EQ(ch, iter)) && GET_OBJ_REQUIRES_QUEST(obj) != NOTHING && !is_on_quest(ch, GET_OBJ_REQUIRES_QUEST(obj))) {
			act("You lose $p because you are not on the right quest.", FALSE, ch, obj, NULL, TO_CHAR);
			extract_obj(obj);
		}
	}
	
	LL_FOREACH_SAFE2(ch->carrying, obj, next_obj, next_content) {
		if (GET_OBJ_REQUIRES_QUEST(obj) != NOTHING && !is_on_quest(ch, GET_OBJ_REQUIRES_QUEST(obj))) {
			act("You lose $p because you are not on the right quest.", FALSE, ch, obj, NULL, TO_CHAR);
			extract_obj(obj);
		}
	}
}


/**
* Silently removes all items related to the quest.
*
* Unlike the all-quest-items version, this one DOES apply to immortals because
* they ended the quest.
*
* @param char_data *ch The player to take items from.
* @param any_vnum vnum The quest to remove items for.
*/
void remove_quest_items_by_quest(char_data *ch, any_vnum vnum) {
	obj_data *obj, *next_obj;
	int iter;
	
	for (iter = 0; iter < NUM_WEARS; ++iter) {
		if ((obj = GET_EQ(ch, iter)) && GET_OBJ_REQUIRES_QUEST(obj) == vnum) {
			extract_obj(obj);
		}
	}
	
	LL_FOREACH_SAFE2(ch->carrying, obj, next_obj, next_content) {
		if (GET_OBJ_REQUIRES_QUEST(obj) == vnum) {
			extract_obj(obj);
		}
	}
}


/**
* Copies entries from one list into another, only if they are not already in
* the to_list.
*
* @param struct quest_giver **to_list A pointer to the destination list.
* @param struct quest_giver *from_list The list to copy from.
*/
void smart_copy_quest_givers(struct quest_giver **to_list, struct quest_giver *from_list) {
	struct quest_giver *iter, *search, *giver;
	bool found;
	
	LL_FOREACH(from_list, iter) {
		// ensure not already in list
		found = FALSE;
		LL_FOREACH(*to_list, search) {
			if (search->type == iter->type && search->vnum == iter->vnum) {
				found = TRUE;
				break;
			}
		}
		
		// add it
		if (!found) {
			CREATE(giver, struct quest_giver, 1);
			giver->type = iter->type;
			giver->vnum = iter->vnum;
			LL_APPEND(*to_list, giver);
		}
	}
}


/**
* Copies entries from one list into another, only if they are not already in
* the to_list.
*
* @param struct quest_reward **to_list A pointer to the destination list.
* @param struct quest_reward *from_list The list to copy from.
*/
void smart_copy_quest_rewards(struct quest_reward **to_list, struct quest_reward *from_list) {
	struct quest_reward *iter, *search, *reward;
	bool found;
	
	LL_FOREACH(from_list, iter) {
		// ensure not already in list
		found = FALSE;
		LL_FOREACH(*to_list, search) {
			if (search->type == iter->type && search->amount == iter->amount && search->vnum == iter->vnum) {
				found = TRUE;
				break;
			}
		}
		
		// add it
		if (!found) {
			CREATE(reward, struct quest_reward, 1);
			reward->type = iter->type;
			reward->amount = iter->amount;
			reward->vnum = iter->vnum;
			LL_APPEND(*to_list, reward);
		}
	}
}


/**
* Copies entries from one list into another, only if they are not already in
* the to_list.
*
* @param struct quest_task **to_list A pointer to the destination list.
* @param struct quest_task *from_list The list to copy from.
*/
void smart_copy_quest_tasks(struct quest_task **to_list, struct quest_task *from_list) {
	struct quest_task *iter, *search, *task;
	bool found;
	
	LL_FOREACH(from_list, iter) {
		// ensure not already in list
		found = FALSE;
		LL_FOREACH(*to_list, search) {
			if (search->type == iter->type && search->vnum == iter->vnum) {
				found = TRUE;
				break;
			}
		}
		
		// add it
		if (!found) {
			CREATE(task, struct quest_task, 1);
			*task = *iter;
			task->next = NULL;
			LL_APPEND(*to_list, task);
		}
	}
}


 //////////////////////////////////////////////////////////////////////////////
//// LOOKUP HANDLERS /////////////////////////////////////////////////////////


/**
* Processes all the quest givers into lookup hint tables.
*
* @param quest_data *quest The quest to update givers for.
* @param bool add If TRUE, adds quest lookup hints. If FALSE, removes them.
*/
void add_or_remove_all_quest_lookups_for(quest_data *quest, bool add) {
	struct quest_giver *list[2], *giver;
	room_template *rmt;
	char_data *mob;
	bld_data *bld;
	obj_data *obj;
	int iter;
	
	if (!quest) {	// somehow
		return;
	}
	
	// work on 2 lists
	list[0] = QUEST_STARTS_AT(quest);
	list[1] = QUEST_ENDS_AT(quest);
	
	for (iter = 0; iter < 2; ++iter) {
		LL_FOREACH(list[iter], giver) {
			// QG_x
			switch (giver->type) {
				case QG_BUILDING: {
					if ((bld = building_proto(giver->vnum))) {
						if (add) {
							add_quest_lookup(&GET_BLD_QUEST_LOOKUPS(bld), quest);
						}
						else {
							remove_quest_lookup(&GET_BLD_QUEST_LOOKUPS(bld), quest);
						}
						// does not require live update
					}
					break;
				}
				case QG_MOBILE: {
					if ((mob = mob_proto(giver->vnum))) {
						if (add) {
							add_quest_lookup(&MOB_QUEST_LOOKUPS(mob), quest);
						}
						else {
							remove_quest_lookup(&MOB_QUEST_LOOKUPS(mob), quest);
						}
						update_mob_quest_lookups(GET_MOB_VNUM(mob));
					}
					break;
				}
				case QG_OBJECT: {
					if ((obj = obj_proto(giver->vnum))) {
						if (add) {
							add_quest_lookup(&GET_OBJ_QUEST_LOOKUPS(obj), quest);
						}
						else {
							remove_quest_lookup(&GET_OBJ_QUEST_LOOKUPS(obj), quest);
						}
						update_obj_quest_lookups(GET_OBJ_VNUM(obj));
					}
					break;
				}
				case QG_ROOM_TEMPLATE: {
					if ((rmt = room_template_proto(giver->vnum))) {
						if (add) {
							add_quest_lookup(&GET_RMT_QUEST_LOOKUPS(rmt), quest);
						}
						else {
							remove_quest_lookup(&GET_RMT_QUEST_LOOKUPS(rmt), quest);
						}
						// does not require live update
					}
					break;
				}
			}
		}
	}
}


/**
* Adds a quest lookup hint to a list (e.g. on a mob).
*
* Note: For mob/obj quests, run update_mob_quest_lookups() or
* update_obj_quest_lookups() after this.
*
* @param struct quest_lookup **list A pointer to the list to add to.
* @param quest_data *quest The quest to add.
*/
void add_quest_lookup(struct quest_lookup **list, quest_data *quest) {
	struct quest_lookup *ql;
	bool found = FALSE;
	
	if (list && quest) {
		// no dupes
		LL_FOREACH(*list, ql) {
			if (ql->quest == quest) {
				found = TRUE;
			}
		}
		
		if (!found) {
			CREATE(ql, struct quest_lookup, 1);
			ql->quest = quest;
			LL_PREPEND(*list, ql);
		}
	}
}


/**
* Builds all the quest lookup tables on startup.
*/
void build_all_quest_lookups(void) {
	quest_data *quest, *next_quest;
	HASH_ITER(hh, quest_table, quest, next_quest) {
		add_or_remove_all_quest_lookups_for(quest, TRUE);
	}
}


/**
* Adds a quest lookup hint to a list (e.g. on a mob).
*
* Note: For mob/obj quests, run update_mob_quest_lookups() or
* update_obj_quest_lookups() after this.
*
* @param struct quest_lookup **list A pointer to the list to add to.
* @param quest_data *quest The quest to add.
* @return bool TRUE if it removed an entry, FALSE for no matches.
*/
bool remove_quest_lookup(struct quest_lookup **list, quest_data *quest) {
	struct quest_lookup *ql, *next_ql;
	bool any = FALSE;
	
	if (list && *list && quest) {
		LL_FOREACH_SAFE(*list, ql, next_ql) {
			if (ql->quest == quest) {
				LL_DELETE(*list, ql);
				free(ql);
				any = TRUE;
			}
		}
	}
	
	return any;
}


/**
* Fixes quest lookup pointers on live copies of mobs -- this should ALWAYS
* point to the proto.
*/
void update_mob_quest_lookups(mob_vnum vnum) {
	char_data *proto, *mob;
	
	if (!(proto = mob_proto(vnum))) {
		return;
	}
	
	LL_FOREACH(character_list, mob) {
		if (IS_NPC(mob) && GET_MOB_VNUM(mob) == vnum) {
			// re-set the pointer
			MOB_QUEST_LOOKUPS(mob) = MOB_QUEST_LOOKUPS(proto);
		}
	}
}


/**
* Fixes quest lookup pointers on live copies of objs -- this should ALWAYS
* point to the proto.
*/
void update_obj_quest_lookups(obj_vnum vnum) {
	obj_data *proto, *obj;
	
	if (!(proto = obj_proto(vnum))) {
		return;
	}
	
	LL_FOREACH(object_list, obj) {
		if (GET_OBJ_VNUM(obj) == vnum) {
			// re-set the pointer
			GET_OBJ_QUEST_LOOKUPS(obj) = GET_OBJ_QUEST_LOOKUPS(proto);
		}
	}
}


 //////////////////////////////////////////////////////////////////////////////
//// QUEST HANDLERS //////////////////////////////////////////////////////////

/**
* @param char_data *ch Any player playing.
* @param char_data *mob Any mob.
* @param struct quest_temp_list **build_list Optional: Builds a temp list of quests available.
* @return bool TRUE if the mob has a quest the character can get; FALSE otherwise.
*/
bool can_get_quest_from_mob(char_data *ch, char_data *mob, struct quest_temp_list **build_list) {
	struct instance_data *inst;
	struct quest_lookup *ql;
	bool any = FALSE;
	
	if (IS_NPC(ch) || !MOB_QUEST_LOOKUPS(mob) || !CAN_SEE(ch, mob)) {
		return FALSE;
	}
	
	LL_FOREACH(MOB_QUEST_LOOKUPS(mob), ql) {
		// make sure they're a giver
		if (!find_quest_giver_in_list(QUEST_STARTS_AT(ql->quest), QG_MOBILE, GET_MOB_VNUM(mob))) {
			continue;
		}
		// already on quest?
		if (is_on_quest(ch, QUEST_VNUM(ql->quest))) {
			continue;
		}
		
		// success
		inst = (MOB_INSTANCE_ID(mob) != NOTHING ? get_instance_by_id(MOB_INSTANCE_ID(mob)) : NULL);
		
		// pre-reqs?
		if (char_meets_prereqs(ch, ql->quest, inst)) {
			if (build_list) {
				any = TRUE;
				add_to_quest_temp_list(build_list, ql->quest, inst);
			}
			else {
				// only need 1
				return TRUE;
			}
		}
	}
	
	// nope
	return any;
}


/**
* @param char_data *ch Any player playing.
* @param obj_data *obj Any obj.
* @param struct quest_temp_list **build_list Optional: Builds a temp list of quests available.
* @return bool TRUE if the obj has a quest the character can get; FALSE otherwise.
*/
bool can_get_quest_from_obj(char_data *ch, obj_data *obj, struct quest_temp_list **build_list) {
	extern room_data *obj_room(obj_data *obj);
	
	struct instance_data *inst;
	struct quest_lookup *ql;
	bool any = FALSE;
	room_data *room;
	
	if (IS_NPC(ch) || !GET_OBJ_QUEST_LOOKUPS(obj) || !CAN_SEE_OBJ(ch, obj) || !bind_ok(obj, ch)) {
		return FALSE;
	}
	
	LL_FOREACH(GET_OBJ_QUEST_LOOKUPS(obj), ql) {
		// make sure they're a giver
		if (!find_quest_giver_in_list(QUEST_STARTS_AT(ql->quest), QG_OBJECT, GET_OBJ_VNUM(obj))) {
			continue;
		}
		// already on quest?
		if (is_on_quest(ch, QUEST_VNUM(ql->quest))) {
			continue;
		}
		
		// success
		room = obj_room(obj);
		inst = (room ? find_instance_by_room(room, FALSE) : NULL);
		
		// pre-reqs?
		if (char_meets_prereqs(ch, ql->quest, inst)) {
			if (build_list) {
				any = TRUE;
				add_to_quest_temp_list(build_list, ql->quest, inst);
			}
			else {
				// only need 1
				return TRUE;
			}
		}
	}
	
	// nope
	return any;
}


/**
* @param char_data *ch Any player playing.
* @param room_data *room Any room.
* @param struct quest_temp_list **build_list Optional: Builds a temp list of quests available.
* @return bool TRUE if the room has a quest the character can get; FALSE otherwise.
*/
bool can_get_quest_from_room(char_data *ch, room_data *room, struct quest_temp_list **build_list) {
	struct quest_lookup *ql, *list[2];
	struct instance_data *inst;
	bool any = FALSE;
	int iter;
	
	if (IS_NPC(ch)) {
		return FALSE;
	}
	
	// two places to look
	list[0] = GET_BUILDING(room) ? GET_BLD_QUEST_LOOKUPS(GET_BUILDING(room)) : NULL;
	list[1] = GET_ROOM_TEMPLATE(room) ? GET_RMT_QUEST_LOOKUPS(GET_ROOM_TEMPLATE(room)) : NULL;
	
	for (iter = 0; iter < 2; ++iter) {
		LL_FOREACH(list[iter], ql) {
			// make sure they're a giver
			if (iter == 0 && !find_quest_giver_in_list(QUEST_STARTS_AT(ql->quest), QG_BUILDING, GET_BLD_VNUM(GET_BUILDING(room)))) {
				continue;
			}
			if (iter == 1 && !find_quest_giver_in_list(QUEST_STARTS_AT(ql->quest), QG_ROOM_TEMPLATE, GET_RMT_VNUM(GET_ROOM_TEMPLATE(room)))) {
				continue;
			}
			// already on quest?
			if (is_on_quest(ch, QUEST_VNUM(ql->quest))) {
				continue;
			}
			
			// success
			inst = (room ? find_instance_by_room(room, FALSE) : NULL);
			
			// pre-reqs?
			if (char_meets_prereqs(ch, ql->quest, inst)) {
				if (build_list) {
					any = TRUE;
					add_to_quest_temp_list(build_list, ql->quest, inst);
				}
				else {
					// only need 1
					return TRUE;
				}
			}
		}
	}
	
	// nope
	return any;
}


/**
* @param char_data *ch Any player playing.
* @param char_data *mob Any mob.
* @param struct quest_temp_list **build_list Optional: Builds a temp list of quests available.
* @return bool TRUE if the player has finished a quest that the mob accepts; FALSE otherwise.
*/
bool can_turn_quest_in_to_mob(char_data *ch, char_data *mob, struct quest_temp_list **build_list) {
	struct player_quest *pq;
	struct quest_lookup *ql;
	int complete, total;
	bool any = FALSE;
	
	if (IS_NPC(ch) || !MOB_QUEST_LOOKUPS(mob) || !CAN_SEE(ch, mob)) {
		return FALSE;
	}
	
	LL_FOREACH(MOB_QUEST_LOOKUPS(mob), ql) {
		// make sure they're a giver
		if (!find_quest_giver_in_list(QUEST_ENDS_AT(ql->quest), QG_MOBILE, GET_MOB_VNUM(mob))) {
			continue;
		}
		// are they on quest?
		if (!(pq = is_on_quest(ch, QUEST_VNUM(ql->quest)))) {
			continue;
		}
		
		count_quest_tasks(pq, &complete, &total);
		if (complete < total) {
			continue;
		}
		
		// success!
		if (build_list) {
			any = TRUE;
			add_to_quest_temp_list(build_list, ql->quest, NULL);
		}
		else {
			return TRUE;
		}
	}
	
	// nope
	return any;
}


/**
* @param char_data *ch Any player playing.
* @param obj_data *obj Any obj.
* @param struct quest_temp_list **build_list Optional: Builds a temp list of quests available.
* @return bool TRUE if the player has a quest complete that the obj ends; FALSE otherwise.
*/
bool can_turn_quest_in_to_obj(char_data *ch, obj_data *obj, struct quest_temp_list **build_list) {
	struct player_quest *pq;
	struct quest_lookup *ql;
	int complete, total;
	bool any = FALSE;
	
	if (IS_NPC(ch) || !GET_OBJ_QUEST_LOOKUPS(obj) || !CAN_SEE_OBJ(ch, obj) || !bind_ok(obj, ch)) {
		return FALSE;
	}
	
	LL_FOREACH(GET_OBJ_QUEST_LOOKUPS(obj), ql) {
		// make sure they're a giver
		if (!find_quest_giver_in_list(QUEST_ENDS_AT(ql->quest), QG_OBJECT, GET_OBJ_VNUM(obj))) {
			continue;
		}
		// are they on quest?
		if (!(pq = is_on_quest(ch, QUEST_VNUM(ql->quest)))) {
			continue;
		}
		
		count_quest_tasks(pq, &complete, &total);
		if (complete < total) {
			continue;
		}
		
		// success!
		if (build_list) {
			any = TRUE;
			add_to_quest_temp_list(build_list, ql->quest, NULL);
		}
		else {
			return TRUE;
		}
	}
	
	// nope
	return any;
}


/**
* @param char_data *ch Any player playing.
* @param room_data *room Any room.
* @param struct quest_temp_list **build_list Optional: Builds a temp list of quests available.
* @return bool TRUE if the player has finished a quest that the room ends; FALSE otherwise.
*/
bool can_turn_quest_in_to_room(char_data *ch, room_data *room, struct quest_temp_list **build_list) {
	struct quest_lookup *ql, *list[2];
	int iter, complete, total;
	struct player_quest *pq;
	bool any = FALSE;
	
	if (IS_NPC(ch)) {
		return FALSE;
	}
	
	// two places to look
	list[0] = GET_BUILDING(room) ? GET_BLD_QUEST_LOOKUPS(GET_BUILDING(room)) : NULL;
	list[1] = GET_ROOM_TEMPLATE(room) ? GET_RMT_QUEST_LOOKUPS(GET_ROOM_TEMPLATE(room)) : NULL;
	
	for (iter = 0; iter < 2; ++iter) {
		LL_FOREACH(list[iter], ql) {
			// make sure they're a giver
			if (iter == 0 && !find_quest_giver_in_list(QUEST_ENDS_AT(ql->quest), QG_BUILDING, GET_BLD_VNUM(GET_BUILDING(room)))) {
				continue;
			}
			if (iter == 1 && !find_quest_giver_in_list(QUEST_ENDS_AT(ql->quest), QG_ROOM_TEMPLATE, GET_RMT_VNUM(GET_ROOM_TEMPLATE(room)))) {
				continue;
			}
			// are they on quest?
			if (!(pq = is_on_quest(ch, QUEST_VNUM(ql->quest)))) {
				continue;
			}
		
			count_quest_tasks(pq, &complete, &total);
			if (complete < total) {
				continue;
			}
		
			// success!
			if (build_list) {
				any = TRUE;
				add_to_quest_temp_list(build_list, ql->quest, NULL);
			}
			else {
				return TRUE;
			}
		}
	}
	
	// nope
	return any;
}


/**
* @param char_data *ch Any player.
* @param quest_data *quest The quest to check.
* @param struct instance_data *instance Optional; If the quest is offered in/from an instance.
* @return bool TRUE if the player can get the quest.
*/
bool char_meets_prereqs(char_data *ch, quest_data *quest, struct instance_data *instance) {
	struct player_completed_quest *completed;
	struct quest_task *task;
	bool ok = TRUE;
	// needs to know instance/adventure
	
	// sanitation
	if (!ch || !quest || IS_NPC(ch)) {
		return FALSE;
	}
	
	// only immortals see in-dev quests
	if (QUEST_FLAGGED(quest, QST_IN_DEVELOPMENT) && !IS_IMMORTAL(ch)) {
		return FALSE;
	}
	
	// check repeatability
	if ((completed = has_completed_quest(ch, QUEST_VNUM(quest)))) {
		if (QUEST_REPEATABLE_AFTER(quest) >= 0 && completed->last_completed + (QUEST_REPEATABLE_AFTER(quest) * SECS_PER_REAL_MIN) <= time(0)) {
			// repeat time: ok
		}
		else if (QUEST_FLAGGED(quest, QST_REPEAT_PER_INSTANCE) && (completed->last_adventure != (instance ? GET_ADV_VNUM(instance->adventure) : NOTHING) || completed->last_instance_id != (instance ? instance->id : NOTHING))) {
			// repeat per instance: ok (different instance)
		}
		else {
			// not repeatable
			ok = FALSE;
		}
	}
	
	// check prereqs
	LL_FOREACH(QUEST_PREREQS(quest), task) {
		// early exit
		if (!ok) {
			break;
		}
		
		// QT_x: only tasks that can be prereqs
		switch(task->type) {
			case QT_COMPLETED_QUEST: {
				if (!has_completed_quest(ch, task->vnum)) {
					ok = FALSE;
				}
				break;
			}
			case QT_GET_COMPONENT: {
				struct resource_data *res = NULL;
				add_to_resource_list(&res, RES_COMPONENT, task->vnum, task->needed, task->misc);
				if (!has_resources(ch, res, FALSE, FALSE)) {
					ok = FALSE;
				}
				free_resource_list(res);
				break;
			}
			case QT_GET_OBJECT: {
				struct resource_data *res = NULL;
				add_to_resource_list(&res, RES_OBJECT, task->vnum, task->needed, 0);
				if (!has_resources(ch, res, FALSE, FALSE)) {
					ok = FALSE;
				}
				free_resource_list(res);
				break;
			}
			case QT_NOT_COMPLETED_QUEST: {
				if (has_completed_quest(ch, task->vnum)) {
					ok = FALSE;
				}
				break;
			}
			case QT_NOT_ON_QUEST: {
				if (is_on_quest(ch, task->vnum)) {
					ok = FALSE;
				}
				break;
			}
			case QT_OWN_BUILDING: {
				if (!GET_LOYALTY(ch) || count_owned_buildings(GET_LOYALTY(ch), task->vnum) < task->needed) {
					ok = FALSE;
				}
				break;
			}
			case QT_OWN_VEHICLE: {
				if (!GET_LOYALTY(ch) || count_owned_vehicles(GET_LOYALTY(ch), task->vnum) < task->needed) {
					ok = FALSE;
				}
				break;
			}
			case QT_SKILL_LEVEL_OVER: {
				if (get_skill_level(ch, task->vnum) < task->needed) {
					ok = FALSE;
				}
				break;
			}
			case QT_SKILL_LEVEL_UNDER: {
				if (get_skill_level(ch, task->vnum) > task->needed) {
					ok = FALSE;
				}
				break;
			}
			case QT_HAVE_ABILITY: {
				if (!has_ability(ch, task->vnum)) {
					ok = FALSE;
				}
				break;
			}
			
			// some types do not support pre-reqs
			case QT_KILL_MOB:
			case QT_KILL_MOB_FLAGGED:
			case QT_TRIGGERED:
			case QT_VISIT_BUILDING:
			case QT_VISIT_ROOM_TEMPLATE:
			case QT_VISIT_SECTOR:
			default: {
				break;
			}
		}
	}
	
	return ok;
}


/**
* @param char_data *ch Any player.
* @param quest_vnum quest The quest to check.
* @return struct player_completed_quest* Returns completion data (TRUE) if the player has completed the quest; NULL (FALSE) otherwise.
*/
struct player_completed_quest *has_completed_quest(char_data *ch, any_vnum quest) {
	struct player_completed_quest *pcq;
	
	if (IS_NPC(ch)) {
		return FALSE;
	}
	
	HASH_FIND_INT(GET_COMPLETED_QUESTS(ch), &quest, pcq);
	return pcq;
}


/**
* @param char_data *ch Any player.
* @param quest_vnum quest The quest to check.
* @return struct player_quest *The player's quest data (TRUE) if on the quest, NULL (FALSE) if not.
*/
struct player_quest *is_on_quest(char_data *ch, any_vnum quest) {
	struct player_quest *pq;
	
	if (IS_NPC(ch)) {
		return NULL;
	}
	
	LL_SEARCH_SCALAR(GET_QUESTS(ch), pq, vnum, quest);
	return pq;
}


 //////////////////////////////////////////////////////////////////////////////
//// QUEST TRACKERS //////////////////////////////////////////////////////////

/**
* Quest Tracker: ch gains or loses ability
*
* @param char_data *ch The player.
* @param any_vnum abil The ability vnum.
*/
void qt_change_ability(char_data *ch, any_vnum abil) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_HAVE_ABILITY && task->vnum == abil) {
				task->current = (has_ability(ch, abil) ? task->needed : 0);
			}
		}
	}
}


/**
* Quest Tracker: ch gains or loses skill
*
* @param char_data *ch The player.
* @param any_vnum skl The skill vnum.
*/
void qt_change_skill_level(char_data *ch, any_vnum skl) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_SKILL_LEVEL_OVER && task->vnum == skl) {
				task->current = (get_skill_level(ch, skl) >= task->needed ? task->needed : 0);
			}
			else if (task->type == QT_SKILL_LEVEL_UNDER && task->vnum == skl) {
				task->current = (get_skill_level(ch, skl) <= task->needed ? task->needed : -1);	// must set below 0 because 0 is a valid needed
			}
		}
	}
}


/**
* Quest Tracker: ch drops (or otherwise loses) an item
*
* Note: Call this AFTER taking the obj away.
*
* @param char_data *ch The player.
* @param obj_data *obj The item.
*/
void qt_drop_obj(char_data *ch, obj_data *obj) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_GET_COMPONENT && GET_OBJ_CMP_TYPE(obj) == task->vnum && (GET_OBJ_CMP_FLAGS(obj) & task->misc) == task->misc) {
				--task->current;
			}
			else if (task->type == QT_GET_OBJECT && GET_OBJ_VNUM(obj) == task->vnum) {
				--task->current;
			}
			
			// check min
			task->current = MAX(task->current, 0);
		}
	}
}


/**
* This can be used to call several different qt functions on all members of
* an empire online.
*
* @param empire_data *emp The empire to call the function on.
* @param void (*func)(char_data *ch, any_vnum vnum) The function to call.
* @param any_vnum vnum The vnum to pass to the function.
*/
void qt_empire_players(empire_data *emp, void (*func)(char_data *ch, any_vnum vnum), any_vnum vnum) {
	descriptor_data *desc;
	char_data *ch;
	
	if (!emp || !func || vnum == NOTHING) {
		return;
	}
	
	LL_FOREACH(descriptor_list, desc) {
		if (STATE(desc) != CON_PLAYING || !(ch = desc->character)) {
			continue;
		}
		if (GET_LOYALTY(ch) != emp) {
			continue;
		}
		
		// call it
		(func)(ch, vnum);
	}
}


/**
* Quest Tracker: ch gets a building
*
* @param char_data *ch The player.
* @param any_vnum vnum The building vnum.
*/
void qt_gain_building(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_OWN_BUILDING && task->vnum == vnum) {
				++task->current;
			}
		}
	}
}


/**
* Quest Tracker: ch gets a vehicle
*
* @param char_data *ch The player.
* @param any_vnum vnum The vehicle vnum.
*/
void qt_gain_vehicle(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_OWN_VEHICLE && task->vnum == vnum) {
				++task->current;
			}
		}
	}
}


/**
* Quest Tracker: ch obtains an item
*
* @param char_data *ch The player.
* @param obj_data *obj The item.
*/
void qt_get_obj(char_data *ch, obj_data *obj) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_GET_COMPONENT && GET_OBJ_CMP_TYPE(obj) == task->vnum && (GET_OBJ_CMP_FLAGS(obj) & task->misc) == task->misc) {
				++task->current;
			}
			else if (task->type == QT_GET_OBJECT && GET_OBJ_VNUM(obj) == task->vnum) {
				++task->current;
			}
		}
	}
}


/**
* Quest Tracker: ch kills a mob
*
* @param char_data *ch The player.
* @param char_data *mob The mob killed.
*/
void qt_kill_mob(char_data *ch, char_data *mob) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch) || !IS_NPC(mob)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_KILL_MOB_FLAGGED && (MOB_FLAGS(mob) & task->misc) == task->misc) {
				++task->current;
			}
			else if (task->type == QT_KILL_MOB && GET_MOB_VNUM(mob) == task->vnum) {
				++task->current;
			}
		}
	}
}


/**
* Quest Tracker: ch loses/dismantles a building
*
* @param char_data *ch The player.
* @param any_vnum vnum The building vnum.
*/
void qt_lose_building(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_OWN_BUILDING && task->vnum == vnum) {
				--task->current;
			}
			
			// check min
			task->current = MAX(task->current, 0);
		}
	}
}


/**
* Quest Tracker: ch drops/finishes a quest
*
* @param char_data *ch The player.
* @param any_vnum vnum The quest vnum.
*/
void qt_lose_quest(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_NOT_ON_QUEST && task->vnum == vnum) {
				task->current = task->needed;
			}
		}
	}
}


/**
* Quest Tracker: ch loses a vehicle
*
* @param char_data *ch The player.
* @param any_vnum vnum The vehicle vnum.
*/
void qt_lose_vehicle(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_OWN_VEHICLE && task->vnum == vnum) {
				--task->current;
			}
			
			// check min
			task->current = MAX(task->current, 0);
		}
	}
}


/**
* Quest Tracker: ch completes a quest
*
* @param char_data *ch The player.
* @param any_vnum vnum The quest vnum.
*/
void qt_quest_completed(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_COMPLETED_QUEST && task->vnum == vnum) {
				task->current = task->needed;
			}
			else if (task->type == QT_NOT_COMPLETED_QUEST && task->vnum == vnum) {
				task->current = 0;
			}
		}
	}
}


/**
* Quest Tracker: ch starts a quest
*
* @param char_data *ch The player.
* @param any_vnum vnum The quest vnum.
*/
void qt_start_quest(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_NOT_ON_QUEST && task->vnum == vnum) {
				task->current = 0;
			}
		}
	}
}


/**
* Quest Tracker: mark a triggered condition for 1 quest
*
* @param char_data *ch The player.
* @param any_vnum vnum The quest to mark.
*/
void qt_triggered_task(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		if (pq->vnum == vnum) {
			LL_FOREACH(pq->tracker, task) {
				if (task->type == QT_TRIGGERED) {
					task->current = task->needed;
				}
			}
		}
	}
}


/**
* Quest Tracker: ch visits a building
*
* @param char_data *ch The player.
* @param any_vnum vnum The building vnum.
*/
void qt_visit_building(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_VISIT_BUILDING && task->vnum == vnum) {
				task->current = task->needed;
			}
		}
	}
}


/**
* Quest Tracker: ch visits a room template
*
* @param char_data *ch The player.
* @param any_vnum vnum The rmt vnum.
*/
void qt_visit_room_template(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_VISIT_ROOM_TEMPLATE && task->vnum == vnum) {
				task->current = task->needed;
			}
		}
	}
}


/**
* Quest Tracker: ch visits a sector
*
* @param char_data *ch The player.
* @param any_vnum vnum The sector vnum.
*/
void qt_visit_sector(char_data *ch, any_vnum vnum) {
	struct player_quest *pq;
	struct quest_task *task;
	
	if (IS_NPC(ch)) {
		return;
	}
	
	LL_FOREACH(GET_QUESTS(ch), pq) {
		LL_FOREACH(pq->tracker, task) {
			if (task->type == QT_VISIT_SECTOR && task->vnum == vnum) {
				task->current = task->needed;
			}
		}
	}
}


/**
* Quest Tracker: run all 'visit' types on the room.
*
* @param char_data *ch The player.
* @param any_vnum vnum The sector vnum.
*/
void qt_visit_room(char_data *ch, room_data *room) {
	if (IS_NPC(ch)) {
		return;
	}
	
	qt_visit_sector(ch, GET_SECT_VNUM(SECT(room)));
	if (GET_BUILDING(room)) {
		qt_visit_building(ch, GET_BLD_VNUM(GET_BUILDING(room)));
	}
	if (GET_ROOM_TEMPLATE(room)) {
		qt_visit_room_template(ch, GET_RMT_VNUM(GET_ROOM_TEMPLATE(room)));
	}
}


 //////////////////////////////////////////////////////////////////////////////
//// UTILITIES ///////////////////////////////////////////////////////////////

/**
* Checks for common quest problems and reports them to ch.
*
* @param quest_data *quest The item to audit.
* @param char_data *ch The person to report to.
* @return bool TRUE if any problems were reported; FALSE if all good.
*/
bool audit_quest(quest_data *quest, char_data *ch) {
	struct trig_proto_list *tpl;
	struct quest_task *task;
	trig_data *trig;
	bool problem = FALSE;
	
	if (QUEST_FLAGGED(quest, QST_IN_DEVELOPMENT)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "IN-DEVELOPMENT");
		problem = TRUE;
	}
	if (!QUEST_NAME(quest) || !*QUEST_NAME(quest) || !str_cmp(QUEST_NAME(quest), default_quest_name)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "Name not set");
		problem = TRUE;
	}
	if (!isupper(*QUEST_NAME(quest))) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "Name not capitalized");
		problem = TRUE;
	}
	if (!QUEST_DESCRIPTION(quest) || !*QUEST_DESCRIPTION(quest) || !str_cmp(QUEST_DESCRIPTION(quest), default_quest_description)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "Description not set");
		problem = TRUE;
	}
	if (!QUEST_COMPLETE_MSG(quest) || !*QUEST_COMPLETE_MSG(quest) || !str_cmp(QUEST_COMPLETE_MSG(quest), default_quest_complete_msg)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "Complete message not set");
		problem = TRUE;
	}
	
	if (QUEST_MIN_LEVEL(quest) > QUEST_MAX_LEVEL(quest) && QUEST_MAX_LEVEL(quest) != 0) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "Min level higher than max level");
		problem = TRUE;
	}

	// check scripts
	LL_FOREACH(QUEST_SCRIPTS(quest), tpl) {
		if (!(trig = real_trigger(tpl->vnum))) {
			olc_audit_msg(ch, QUEST_VNUM(quest), "Non-existent trigger %d", tpl->vnum);
			problem = TRUE;
			continue;
		}
		if (trig->attach_type != WLD_TRIGGER) {
			olc_audit_msg(ch, QUEST_VNUM(quest), "Incorrect trigger type (trg %d)", tpl->vnum);
			problem = TRUE;
		}
	}
	
	if (!QUEST_STARTS_AT(quest)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "No start locations");
		problem = TRUE;
	}
	if (!QUEST_ENDS_AT(quest)) {
		olc_audit_msg(ch, QUEST_VNUM(quest), "No end locations");
		problem = TRUE;
	}
	
	LL_FOREACH(QUEST_PREREQS(quest), task) {
		// QT_x: types that can't be used as prereqs
		switch (task->type) {
			case QT_KILL_MOB:
			case QT_KILL_MOB_FLAGGED:
			case QT_TRIGGERED:
			case QT_VISIT_BUILDING:
			case QT_VISIT_ROOM_TEMPLATE:
			case QT_VISIT_SECTOR: {
				olc_audit_msg(ch, QUEST_VNUM(quest), "Invalid prereq type %s", quest_tracker_types[task->type]);
				problem = TRUE;
				break;
			}
		}
	}
	
	return problem;
}


/**
* Deletes entries by type+vnum.
*
* @param struct quest_giver **list A pointer to the list to delete from.
* @param int type QG_ type.
* @param any_vnum vnum The vnum to remove.
* @return bool TRUE if the type+vnum was removed from the list. FALSE if not.
*/
bool delete_quest_giver_from_list(struct quest_giver **list, int type, any_vnum vnum) {
	struct quest_giver *iter, *next_iter;
	bool any = FALSE;
	
	LL_FOREACH_SAFE(*list, iter, next_iter) {
		if (iter->type == type && iter->vnum == vnum) {
			any = TRUE;
			LL_DELETE(*list, iter);
			free(iter);
		}
	}
	
	return any;
}


/**
* Deletes entries by type+vnum.
*
* @param struct quest_reward **list A pointer to the list to delete from.
* @param int type QG_ type.
* @param any_vnum vnum The vnum to remove.
* @return bool TRUE if the type+vnum was removed from the list. FALSE if not.
*/
bool delete_quest_reward_from_list(struct quest_reward **list, int type, any_vnum vnum) {
	struct quest_reward *iter, *next_iter;
	bool any = FALSE;
	
	LL_FOREACH_SAFE(*list, iter, next_iter) {
		if (iter->type == type && iter->vnum == vnum) {
			any = TRUE;
			LL_DELETE(*list, iter);
			free(iter);
		}
	}
	
	return any;
}


/**
* Deletes entries by type+vnum.
*
* @param struct quest_task **list A pointer to the list to delete from.
* @param int type QG_ type.
* @param any_vnum vnum The vnum to remove.
* @return bool TRUE if the type+vnum was removed from the list. FALSE if not.
*/
bool delete_quest_task_from_list(struct quest_task **list, int type, any_vnum vnum) {
	struct quest_task *iter, *next_iter;
	bool any = FALSE;
	
	LL_FOREACH_SAFE(*list, iter, next_iter) {
		if (iter->type == type && iter->vnum == vnum) {
			any = TRUE;
			LL_DELETE(*list, iter);
			free(iter);
		}
	}
	
	return any;
}


/**
* @param struct quest_giver *list A list to search.
* @param int type QG_ type.
* @param any_vnum vnum The vnum to look for.
* @return bool TRUE if the type+vnum is in the list. FALSE if not.
*/
bool find_quest_giver_in_list(struct quest_giver *list, int type, any_vnum vnum) {
	struct quest_giver *iter;
	LL_FOREACH(list, iter) {
		if (iter->type == type && iter->vnum == vnum) {
			return TRUE;
		}
	}
	return FALSE;
}


/**
* @param struct quest_reward *list A list to search.
* @param int type QT_ type.
* @param any_vnum vnum The vnum to look for.
* @return bool TRUE if the type+vnum is in the list. FALSE if not.
*/
bool find_quest_reward_in_list(struct quest_reward *list, int type, any_vnum vnum) {
	struct quest_reward *iter;
	LL_FOREACH(list, iter) {
		if (iter->type == type && iter->vnum == vnum) {
			return TRUE;
		}
	}
	return FALSE;
}


/**
* @param struct quest_task *list A list to search.
* @param int type QT_ type.
* @param any_vnum vnum The vnum to look for.
* @return bool TRUE if the type+vnum is in the list. FALSE if not.
*/
bool find_quest_task_in_list(struct quest_task *list, int type, any_vnum vnum) {
	struct quest_task *iter;
	LL_FOREACH(list, iter) {
		if (iter->type == type && iter->vnum == vnum) {
			return TRUE;
		}
	}
	return FALSE;
}


/**
* For the .list command.
*
* @param quest_data *quest The thing to list.
* @param bool detail If TRUE, provide additional details
* @return char* The line to show (without a CRLF).
*/
char *list_one_quest(quest_data *quest, bool detail) {
	static char output[MAX_STRING_LENGTH];
	
	if (detail) {
		snprintf(output, sizeof(output), "[%5d] %s", QUEST_VNUM(quest), QUEST_NAME(quest));
	}
	else {
		snprintf(output, sizeof(output), "[%5d] %s", QUEST_VNUM(quest), QUEST_NAME(quest));
	}
		
	return output;
}


/**
* Searches for all uses of a quest and displays them.
*
* @param char_data *ch The player.
* @param any_vnum vnum The quest vnum.
*/
void olc_search_quest(char_data *ch, any_vnum vnum) {
	char buf[MAX_STRING_LENGTH];
	quest_data *quest = quest_proto(vnum);
	quest_data *qiter, *next_qiter;
	int size, found;
	bool any;
	
	if (!quest) {
		msg_to_char(ch, "There is no quest %d.\r\n", vnum);
		return;
	}
	
	found = 0;
	size = snprintf(buf, sizeof(buf), "Occurrences of quest %d (%s):\r\n", vnum, QUEST_NAME(quest));
	
	// on other quests
	HASH_ITER(hh, quest_table, qiter, next_qiter) {
		if (size >= sizeof(buf)) {
			break;
		}
		// QR_x, QT_x: quest types
		any = find_quest_task_in_list(QUEST_TASKS(qiter), QT_COMPLETED_QUEST, vnum);
		any |= find_quest_task_in_list(QUEST_PREREQS(qiter), QT_COMPLETED_QUEST, vnum);
		any |= find_quest_task_in_list(QUEST_TASKS(qiter), QT_NOT_COMPLETED_QUEST, vnum);
		any |= find_quest_task_in_list(QUEST_PREREQS(qiter), QT_NOT_COMPLETED_QUEST, vnum);
		any |= find_quest_task_in_list(QUEST_TASKS(qiter), QT_NOT_ON_QUEST, vnum);
		any |= find_quest_task_in_list(QUEST_PREREQS(qiter), QT_NOT_ON_QUEST, vnum);
		any |= find_quest_reward_in_list(QUEST_REWARDS(qiter), QR_QUEST_CHAIN, vnum);
		
		if (any) {
			++found;
			size += snprintf(buf + size, sizeof(buf) - size, "QST [%5d] %s\r\n", QUEST_VNUM(qiter), QUEST_NAME(qiter));
		}
	}
	
	if (found > 0) {
		size += snprintf(buf + size, sizeof(buf) - size, "%d location%s shown\r\n", found, PLURAL(found));
	}
	else {
		size += snprintf(buf + size, sizeof(buf) - size, " none\r\n");
	}
	
	page_string(ch->desc, buf, TRUE);
}


/**
* Processing for quest-givers (starts at, ends at).
*
* @param char_data *ch The player using OLC.
* @param char *argument The full argument after the command.
* @param struct quest_giver **list A pointer to the list we're adding/changing.
* @param char *command The command used by the player (starts, ends).
*/
void qedit_process_quest_givers(char_data *ch, char *argument, struct quest_giver **list, char *command) {
	char cmd_arg[MAX_INPUT_LENGTH], field_arg[MAX_INPUT_LENGTH];
	char num_arg[MAX_INPUT_LENGTH], type_arg[MAX_INPUT_LENGTH];
	char vnum_arg[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH];
	struct quest_giver *giver, *iter, *change, *copyfrom;
	int findtype, num, type;
	bool found, ok;
	any_vnum vnum;
	
	argument = any_one_arg(argument, cmd_arg);	// add/remove/change/copy
	
	if (is_abbrev(cmd_arg, "copy")) {
		// usage: qedit starts/ends copy <from type> <from vnum> <starts/ends>
		argument = any_one_arg(argument, type_arg);	// just "quest" for now
		argument = any_one_arg(argument, vnum_arg);	// any vnum for that type
		argument = any_one_arg(argument, field_arg);	// starts/ends
		
		if (!*type_arg || !*vnum_arg) {
			msg_to_char(ch, "Usage: %s copy <from type> <from vnum> [starts | ends]\r\n", command);
		}
		else if ((findtype = find_olc_type(type_arg)) == 0) {
			msg_to_char(ch, "Unknown olc type '%s'.\r\n", type_arg);
		}
		else if (!isdigit(*vnum_arg)) {
			sprintbit(findtype, olc_type_bits, buf, FALSE);
			msg_to_char(ch, "Copy from which %s?\r\n", buf);
		}
		else if ((vnum = atoi(vnum_arg)) < 0) {
			msg_to_char(ch, "Invalid vnum.\r\n");
		}
		else {
			sprintbit(findtype, olc_type_bits, buf, FALSE);
			copyfrom = NULL;
			
			switch (findtype) {
				case OLC_QUEST: {
					// requires starts/ends
					if (!*field_arg || (!is_abbrev(field_arg, "starts") && !is_abbrev(field_arg, "ends"))) {
						msg_to_char(ch, "Copy from the 'starts' or 'ends' list?\r\n");
						return;
					}
					quest_data *from_qst = quest_proto(vnum);
					if (from_qst) {
						copyfrom = (is_abbrev(field_arg, "starts") ? QUEST_STARTS_AT(from_qst) : QUEST_ENDS_AT(from_qst));
					}
					break;
				}
				default: {
					msg_to_char(ch, "You can't copy '%s' from %ss.\r\n", command, buf);
					return;
				}
			}
			
			if (!copyfrom) {
				msg_to_char(ch, "Invalid %s vnum '%s'.\r\n", buf, vnum_arg);
			}
			else {
				smart_copy_quest_givers(list, copyfrom);
				msg_to_char(ch, "Copied '%s' from %s %d.\r\n", command, buf, vnum);
			}
		}
	}	// end 'copy'
	else if (is_abbrev(cmd_arg, "remove")) {
		// usage: qedit starts|ends remove <number | all>
		skip_spaces(&argument);	// only arg is number
		
		if (!*argument) {
			msg_to_char(ch, "Remove which '%s' (number)?\r\n", command);
		}
		else if (!str_cmp(argument, "all")) {
			free_quest_givers(*list);
			*list = NULL;
			msg_to_char(ch, "You remove all the '%s'.\r\n", command);
		}
		else if (!isdigit(*argument) || (num = atoi(argument)) < 1) {
			msg_to_char(ch, "Invalid '%s' number.\r\n", command);
		}
		else {
			found = FALSE;
			LL_FOREACH(*list, iter) {
				if (--num == 0) {
					found = TRUE;
					
					msg_to_char(ch, "You remove the '%s' info for %s %d.\r\n", command, quest_giver_types[iter->type], iter->vnum);
					LL_DELETE(*list, iter);
					free(iter);
					break;
				}
			}
			
			if (!found) {
				msg_to_char(ch, "Invalid '%s' number.\r\n", command);
			}
		}
	}	// end 'remove'
	else if (is_abbrev(cmd_arg, "add")) {
		// usage: qedit starts|ends add <type> <vnum>
		argument = any_one_arg(argument, type_arg);
		argument = any_one_arg(argument, vnum_arg);
		
		if (!*type_arg || !*vnum_arg) {
			msg_to_char(ch, "Usage: %s add <type> <vnum>\r\n", command);
		}
		else if ((type = search_block(type_arg, quest_giver_types, FALSE)) == NOTHING) {
			msg_to_char(ch, "Invalid type '%s'.\r\n", type_arg);
		}
		else if (!isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
			msg_to_char(ch, "Invalid vnum '%s'.\r\n", vnum_arg);
		}
		else {
			// QG_x: validate vnum
			ok = FALSE;
			switch (type) {
				case QG_BUILDING: {
					if (building_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_MOBILE: {
					if (mob_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_OBJECT: {
					if (obj_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_ROOM_TEMPLATE: {
					if (room_template_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_TRIGGER: {
					if (real_trigger(vnum)) {
						ok = TRUE;
					}
					break;
				}
			}
			
			// did we find one? if so, buf is set
			if (!ok) {
				msg_to_char(ch, "Unable to find %s %d.\r\n", quest_giver_types[type], vnum);
				return;
			}
			
			// success
			CREATE(giver, struct quest_giver, 1);
			giver->type = type;
			giver->vnum = vnum;
			
			LL_APPEND(*list, giver);
			msg_to_char(ch, "You add '%s': %s\r\n", command, quest_giver_string(giver, TRUE));
		}
	}	// end 'add'
	else if (is_abbrev(cmd_arg, "change")) {
		// usage: qedit starts|ends change <number> vnum <number>
		argument = any_one_arg(argument, num_arg);
		argument = any_one_arg(argument, field_arg);
		argument = any_one_arg(argument, vnum_arg);
		
		if (!*num_arg || !isdigit(*num_arg) || !*field_arg || !*vnum_arg) {
			msg_to_char(ch, "Usage: %s change <number> vnum <value>\r\n", command);
			return;
		}
		
		// find which one to change
		num = atoi(num_arg);
		change = NULL;
		LL_FOREACH(*list, iter) {
			if (--num == 0) {
				change = iter;
				break;
			}
		}
		
		if (!change) {
			msg_to_char(ch, "Invalid '%s' number.\r\n", command);
		}
		else if (is_abbrev(field_arg, "vnum")) {
			if (!isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
				msg_to_char(ch, "Invalid vnum '%s'.\r\n", vnum_arg);
				return;
			}
			
			// QG_x: validate vnum
			ok = FALSE;
			switch (change->type) {
				case QG_BUILDING: {
					if (building_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_MOBILE: {
					if (mob_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_OBJECT: {
					if (obj_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_ROOM_TEMPLATE: {
					if (room_template_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QG_TRIGGER: {
					if (real_trigger(vnum)) {
						ok = TRUE;
					}
					break;
				}
			}
			
			// did we find one? if so, buf is set
			if (!ok) {
				msg_to_char(ch, "Unable to find %s %d.\r\n", quest_giver_types[change->type], vnum);
				return;
			}
			
			change->vnum = vnum;
			msg_to_char(ch, "Changed '%s' %d to: %s\r\n", command, atoi(num_arg), quest_giver_string(change, TRUE));
		}
		else {
			msg_to_char(ch, "You can only change the vnum.\r\n");
		}
	}	// end 'change'
	else {
		msg_to_char(ch, "Usage: %s add <type> <vnum>\r\n", command);
		msg_to_char(ch, "Usage: %s change <number> vnum <value>\r\n", command);
		msg_to_char(ch, "Usage: %s copy <from type> <from vnum> [starts/ends]\r\n", command);
		msg_to_char(ch, "Usage: %s remove <number | all>\r\n", command);
	}
}


/**
* Sub-processor for quest task args.
*
* @param char_data *ch The player using OLC.
* @param int type QT_ type.
* @param char *argument The remainder of the player's args.
* @param bool find_amount Whether or not to take the 1st arg as amount, if the type requires it.
* @param int *amount A variable to store the amount to.
* @param any_vnum *vnum A variable to store the vnum to.
* @param bitvector_t *misc A variable to store the misc value to.
* @return bool TRUE if the arguments were provided correctly, FALSE if an error was sent.
*/
bool qedit_parse_task_args(char_data *ch, int type, char *argument, bool find_amount, int *amount, any_vnum *vnum, bitvector_t *misc) {
	extern const char *component_flags[];
	extern const char *component_types[];
	extern const bool quest_tracker_amt_type[];
	
	char arg[MAX_INPUT_LENGTH]; 
	bool need_abil = FALSE, need_bld = FALSE, need_component = FALSE;
	bool need_mob = FALSE, need_obj = FALSE, need_quest = FALSE;
	bool need_rmt = FALSE, need_sect = FALSE, need_skill = FALSE;
	bool need_veh = FALSE, need_mob_flags = FALSE;
	
	*amount = 1;
	*vnum = 0;
	*misc = 0;
	
	// QT_x: determine which args we need
	switch (type) {
		case QT_COMPLETED_QUEST:
		case QT_NOT_COMPLETED_QUEST:
		case QT_NOT_ON_QUEST: {
			need_quest = TRUE;
			break;
		}
		case QT_GET_COMPONENT: {
			need_component = TRUE;
			break;
		}
		case QT_GET_OBJECT: {
			need_obj = TRUE;
			break;
		}
		case QT_KILL_MOB: {
			need_mob = TRUE;
			break;
		}
		case QT_KILL_MOB_FLAGGED: {
			need_mob_flags = TRUE;
			break;
		} {
		}
		case QT_OWN_BUILDING: {
			need_bld = TRUE;
			break;
		}
		case QT_OWN_VEHICLE: {
			need_veh = TRUE;
			break;
		}
		case QT_SKILL_LEVEL_OVER:
		case QT_SKILL_LEVEL_UNDER: {
			need_skill = TRUE;
			break;
		}
		case QT_TRIGGERED: {
			break;
		}
		case QT_VISIT_BUILDING: {
			need_bld = TRUE;
			break;
		}
		case QT_VISIT_ROOM_TEMPLATE: {
			need_rmt = TRUE;
			break;
		}
		case QT_VISIT_SECTOR: {
			need_sect = TRUE;
			break;
		}
		case QT_HAVE_ABILITY: {
			need_abil = TRUE;
			break;
		}
	}
	
	// possible args
	if (quest_tracker_amt_type[type] != QT_AMT_NONE && find_amount) {
		argument = any_one_arg(argument, arg);
		if (!*arg || !isdigit(*arg) || (*amount = atoi(arg)) < 0) {
			msg_to_char(ch, "You must provide an amount.\r\n");
			return FALSE;
		}
	}
	
	if (need_abil) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide an ability vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !find_ability_by_vnum(*vnum)) {
			msg_to_char(ch, "Invalid ability vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	
	if (need_bld) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a building vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !building_proto(*vnum)) {
			msg_to_char(ch, "Invalid building vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_component) {
		argument = any_one_arg(argument, arg);
		skip_spaces(&argument);
		if (!*arg) {
			msg_to_char(ch, "You must provide a component type.\r\n");
			return FALSE;
		}
		if ((*vnum = search_block(arg, component_types, FALSE)) == NOTHING) {
			msg_to_char(ch, "Invalid component type '%s'.\r\n", arg);
			return FALSE;
		}
		if (*argument) {
			*misc = olc_process_flag(ch, argument, "component", "", component_flags, NOBITS);
		}
	}
	if (need_mob) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a mob vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !mob_proto(*vnum)) {
			msg_to_char(ch, "Invalid mobile vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_mob_flags) {
		*misc = olc_process_flag(ch, argument, "mob", "", action_bits, NOBITS);
		if (!*misc) {
			msg_to_char(ch, "You must provide mob flags.\r\n");
			return FALSE;
		}
	}
	if (need_obj) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide an object vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !obj_proto(*vnum)) {
			msg_to_char(ch, "Invalid object vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_quest) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a quest vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !quest_proto(*vnum)) {
			msg_to_char(ch, "Invalid quest vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_rmt) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a room template vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !room_template_proto(*vnum)) {
			msg_to_char(ch, "Invalid room template vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_sect) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a sector vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !sector_proto(*vnum)) {
			msg_to_char(ch, "Invalid sector vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_skill) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a skill vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !find_skill_by_vnum(*vnum)) {
			msg_to_char(ch, "Invalid skill vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	if (need_veh) {
		argument = any_one_arg(argument, arg);
		if (!*arg) {
			msg_to_char(ch, "You must provide a vehicle vnum.\r\n");
			return FALSE;
		}
		if (!isdigit(*arg) || (*vnum = atoi(arg)) < 0 || !vehicle_proto(*vnum)) {
			msg_to_char(ch, "Invalid vehicle vnum '%s'.\r\n", arg);
			return FALSE;
		}
	}
	
	// all good
	return TRUE;
}


/**
* Processing for quest-tasks (and pre-reqs).
*
* @param char_data *ch The player using OLC.
* @param char *argument The full argument after the command.
* @param struct quest_t **list A pointer to the list we're adding/changing.
* @param char *command The command used by the player (tasks, prereqs).
*/
void qedit_process_quest_tasks(char_data *ch, char *argument, struct quest_task **list, char *command) {
	char cmd_arg[MAX_INPUT_LENGTH], field_arg[MAX_INPUT_LENGTH];
	char num_arg[MAX_INPUT_LENGTH], type_arg[MAX_INPUT_LENGTH];
	char vnum_arg[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH];
	struct quest_task *task, *iter, *change, *copyfrom;
	int findtype, num, type;
	bitvector_t misc;
	any_vnum vnum;
	bool found;
	
	argument = any_one_arg(argument, cmd_arg);	// add/remove/change/copy
	
	if (is_abbrev(cmd_arg, "copy")) {
		// usage: qedit * copy <from type> <from vnum> <tasks/prereqs>
		argument = any_one_arg(argument, type_arg);	// just "quest" for now
		argument = any_one_arg(argument, vnum_arg);	// any vnum for that type
		argument = any_one_arg(argument, field_arg);	// tasks/prereqs
		
		if (!*type_arg || !*vnum_arg) {
			msg_to_char(ch, "Usage: %ss copy <from type> <from vnum> [tasks | prereqs]\r\n", command);
		}
		else if ((findtype = find_olc_type(type_arg)) == 0) {
			msg_to_char(ch, "Unknown olc type '%s'.\r\n", type_arg);
		}
		else if (!isdigit(*vnum_arg)) {
			sprintbit(findtype, olc_type_bits, buf, FALSE);
			msg_to_char(ch, "Copy from which %s?\r\n", buf);
		}
		else if ((vnum = atoi(vnum_arg)) < 0) {
			msg_to_char(ch, "Invalid vnum.\r\n");
		}
		else {
			sprintbit(findtype, olc_type_bits, buf, FALSE);
			copyfrom = NULL;
			
			switch (findtype) {
				case OLC_QUEST: {
					// requires tasks/preqeqs
					if (!*field_arg || !is_abbrev(field_arg, "tasks")) {
						msg_to_char(ch, "Copy from the 'tasks' or 'prereqs' list?\r\n");
						return;
					}
					quest_data *from_qst = quest_proto(vnum);
					if (from_qst) {
						copyfrom = (is_abbrev(field_arg, "tasks") ? QUEST_TASKS(from_qst) : QUEST_PREREQS(from_qst));
					}
					break;
				}
				default: {
					msg_to_char(ch, "You can't copy %ss from %ss.\r\n", command, buf);
					return;
				}
			}
			
			if (!copyfrom) {
				msg_to_char(ch, "Invalid %s vnum '%s'.\r\n", buf, vnum_arg);
			}
			else {
				smart_copy_quest_tasks(list, copyfrom);
				msg_to_char(ch, "Copied %ss from %s %d.\r\n", command, buf, vnum);
			}
		}
	}	// end 'copy'
	else if (is_abbrev(cmd_arg, "remove")) {
		// usage: qedit * remove <number | all>
		skip_spaces(&argument);	// only arg is number
		
		if (!*argument) {
			msg_to_char(ch, "Remove which %s (number)?\r\n", command);
		}
		else if (!str_cmp(argument, "all")) {
			free_quest_tasks(*list);
			*list = NULL;
			msg_to_char(ch, "You remove all the %ss.\r\n", command);
		}
		else if (!isdigit(*argument) || (num = atoi(argument)) < 1) {
			msg_to_char(ch, "Invalid %s number.\r\n", command);
		}
		else {
			found = FALSE;
			LL_FOREACH(*list, iter) {
				if (--num == 0) {
					found = TRUE;
					
					msg_to_char(ch, "You remove the %s info for: %s\r\n", command, quest_task_string(iter, TRUE));
					LL_DELETE(*list, iter);
					free(iter);
					break;
				}
			}
			
			if (!found) {
				msg_to_char(ch, "Invalid %s number.\r\n", command);
			}
		}
	}	// end 'remove'
	else if (is_abbrev(cmd_arg, "add")) {		
		// usage: qedit * add <type> <vnum>
		argument = any_one_arg(argument, type_arg);
		
		if (!*type_arg) {
			msg_to_char(ch, "Usage: %ss add <type> [amount] [vnum] (see HELP QEDIT TASKS)\r\n", command);
		}
		else if ((type = search_block(type_arg, quest_tracker_types, FALSE)) == NOTHING) {
			msg_to_char(ch, "Invalid type '%s'.\r\n", type_arg);
		}
		else if (!qedit_parse_task_args(ch, type, argument, TRUE, &num, &vnum, &misc)) {
			// sends own error
		}
		else {
			// success
			CREATE(task, struct quest_task, 1);
			task->type = type;
			task->vnum = vnum;
			task->misc = misc;
			task->needed = num;
		
			LL_APPEND(*list, task);
			msg_to_char(ch, "You add %s: %s\r\n", command, quest_task_string(task, TRUE));
		}
	}	// end 'add'
	else if (is_abbrev(cmd_arg, "change")) {
		// usage: qedit * change <number> vnum <number>
		argument = any_one_arg(argument, num_arg);
		argument = any_one_arg(argument, field_arg);
		skip_spaces(&argument);
		
		if (!*num_arg || !isdigit(*num_arg) || !*field_arg) {
			msg_to_char(ch, "Usage: %ss change <number> <amount | vnum> <value>\r\n", command);
			return;
		}
		
		// find which one to change
		num = atoi(num_arg);
		change = NULL;
		LL_FOREACH(*list, iter) {
			if (--num == 0) {
				change = iter;
				break;
			}
		}
		
		if (!change) {
			msg_to_char(ch, "Invalid %s number.\r\n", command);
		}
		else if (is_abbrev(field_arg, "amount")) {
			if (!isdigit(*argument) || (num = atoi(argument)) < 0) {
				msg_to_char(ch, "Invalid amount '%s'.\r\n", argument);
				return;
			}
			else {
				change->needed = num;
				msg_to_char(ch, "You change %s %d to: %s\r\n", command, atoi(num_arg), quest_task_string(change, TRUE));
			}
		}
		else if (is_abbrev(field_arg, "vnum") || is_abbrev(field_arg, "type")) {
			// num is junk here
			if (!qedit_parse_task_args(ch, change->type, argument, FALSE, &num, &vnum, &misc)) {
				// sends own error
			}
			else {
				change->vnum = vnum;
				change->misc = misc;
				msg_to_char(ch, "Changed %s %d to: %s\r\n", command, atoi(num_arg), quest_task_string(change, TRUE));
			}
		}
		else {
			msg_to_char(ch, "You can only change the amount or vnum.\r\n");
		}
	}	// end 'change'
	else if (is_abbrev(cmd_arg, "move")) {
		struct quest_task *to_move, *prev, *a, *b, *a_next, *b_next, iitem;
		bool up;
		
		// usage: tasks move <number> <up | down>
		argument = any_one_arg(argument, num_arg);
		argument = any_one_arg(argument, field_arg);
		up = is_abbrev(field_arg, "up");
		
		if (!*num_arg || !*field_arg) {
			msg_to_char(ch, "Usage: %ss move <number> <up | down>\r\n", command);
		}
		else if (!isdigit(*num_arg) || (num = atoi(num_arg)) < 1) {
			msg_to_char(ch, "Invalid %s number.\r\n", command);
		}
		else if (!is_abbrev(field_arg, "up") && !is_abbrev(field_arg, "down")) {
			msg_to_char(ch, "You must specify whether you're moving it up or down in the list.\r\n");
		}
		else if (up && num == 1) {
			msg_to_char(ch, "You can't move it up; it's already at the top of the list.\r\n");
		}
		else {
			// find the one to move
			to_move = prev = NULL;
			for (task = *list; task && !to_move; task = task->next) {
				if (--num == 0) {
					to_move = task;
				}
				else {
					// store for next iteration
					prev = task;
				}
			}
			
			if (!to_move) {
				msg_to_char(ch, "Invalid %s number.\r\n", command);
			}
			else if (!up && !to_move->next) {
				msg_to_char(ch, "You can't move it down; it's already at the bottom of the list.\r\n");
			}
			else {
				// SUCCESS: "move" them by swapping data
				if (up) {
					a = prev;
					b = to_move;
				}
				else {
					a = to_move;
					b = to_move->next;
				}
				
				// store next pointers
				a_next = a->next;
				b_next = b->next;
				
				// swap data
				iitem = *a;
				*a = *b;
				*b = iitem;
				
				// restore next pointers
				a->next = a_next;
				b->next = b_next;
				
				// message: re-atoi(num_arg) because we destroyed num finding our target
				msg_to_char(ch, "You move %s %d %s.\r\n", command, atoi(num_arg), (up ? "up" : "down"));
			}
		}
	}	// end 'move'
	else {
		msg_to_char(ch, "Usage: %ss add <type> <vnum>\r\n", command);
		msg_to_char(ch, "Usage: %ss change <number> vnum <value>\r\n", command);
		msg_to_char(ch, "Usage: %ss copy <from type> <from vnum> [tasks/prereqs]\r\n", command);
		msg_to_char(ch, "Usage: %ss remove <number | all>\r\n", command);
		msg_to_char(ch, "Usage: %ss move <number> <up | down>\r\n", command);
	}
}


// Simple vnum sorter for the quest hash
int sort_quests(quest_data *a, quest_data *b) {
	return QUEST_VNUM(a) - QUEST_VNUM(b);
}


 //////////////////////////////////////////////////////////////////////////////
//// DATABASE ////////////////////////////////////////////////////////////////

/**
* @param any_vnum vnum Any quest vnum
* @return quest_data* The quest, or NULL if it doesn't exist
*/
quest_data *quest_proto(any_vnum vnum) {
	quest_data *quest;
	
	if (vnum < 0 || vnum == NOTHING) {
		return NULL;
	}
	
	HASH_FIND_INT(quest_table, &vnum, quest);
	return quest;
}


/**
* Puts a quest into the hash table.
*
* @param quest_data *quest The quest data to add to the table.
*/
void add_quest_to_table(quest_data *quest) {
	quest_data *find;
	any_vnum vnum;
	
	if (quest) {
		vnum = QUEST_VNUM(quest);
		HASH_FIND_INT(quest_table, &vnum, find);
		if (!find) {
			HASH_ADD_INT(quest_table, vnum, quest);
			HASH_SORT(quest_table, sort_quests);
		}
	}
}


/**
* Removes a quest from the hash table.
*
* @param quest_data *quest The quest data to remove from the table.
*/
void remove_quest_from_table(quest_data *quest) {
	HASH_DEL(quest_table, quest);
}


/**
* Adds a quest to a temporary list, if it's not already there.
*
* @param struct quest_temp_list **list A pointer to the temporary list to add to.
* @param quest_data *quest The quest to add.
* @param struct instance_data *inst The associated instance for the quest (may be NULL).
*/
void add_to_quest_temp_list(struct quest_temp_list **list, quest_data *quest, struct instance_data *inst) {
	struct quest_temp_list *qtl;
	bool found = FALSE;
	
	LL_FOREACH(*list, qtl) {
		if (qtl->quest == quest) {
			found = TRUE;
			break;
		}
	}
	
	if (!found) {
		CREATE(qtl, struct quest_temp_list, 1);
		qtl->quest = quest;
		qtl->instance = inst;
		LL_PREPEND(*list, qtl);
	}
}


/**
* Initializes a new quest. This clears all memory for it, so set the vnum
* AFTER.
*
* @param quest_data *quest The quest to initialize.
*/
void clear_quest(quest_data *quest) {
	memset((char *) quest, 0, sizeof(quest_data));
	
	QUEST_VNUM(quest) = NOTHING;
	QUEST_REPEATABLE_AFTER(quest) = NOT_REPEATABLE;
}


/**
* @param struct quest_giver *from The list to copy.
* @return struct quest_giver* The copy of the list.
*/
struct quest_giver *copy_quest_givers(struct quest_giver *from) {
	struct quest_giver *el, *iter, *list = NULL, *end = NULL;
	
	LL_FOREACH(from, iter) {
		CREATE(el, struct quest_giver, 1);
		*el = *iter;
		el->next = NULL;
		
		if (end) {
			end->next = el;
		}
		else {
			list = el;
		}
		end = el;
	}
	
	return list;
}


/**
* @param struct quest_reward *from The list to copy.
* @return struct quest_reward* The copy of the list.
*/
struct quest_reward *copy_quest_rewards(struct quest_reward *from) {
	struct quest_reward *el, *iter, *list = NULL, *end = NULL;
	
	LL_FOREACH(from, iter) {
		CREATE(el, struct quest_reward, 1);
		*el = *iter;
		el->next = NULL;
		
		if (end) {
			end->next = el;
		}
		else {
			list = el;
		}
		end = el;
	}
	
	return list;
}


/**
* @param struct quest_task *from The list to copy.
* @return struct quest_task* The copy of the list.
*/
struct quest_task *copy_quest_tasks(struct quest_task *from) {
	struct quest_task *el, *iter, *list = NULL, *end = NULL;
	
	LL_FOREACH(from, iter) {
		CREATE(el, struct quest_task, 1);
		*el = *iter;
		el->next = NULL;
		
		if (end) {
			end->next = el;
		}
		else {
			list = el;
		}
		end = el;
	}
	
	return list;
}


/**
* Frees a player completed-quests hash.
*
* @param struct player_completed_quest **hash A pointer to the hash to free.
*/
void free_player_completed_quests(struct player_completed_quest **hash) {
	struct player_completed_quest *pcq, *next_pcq;
	HASH_ITER(hh, *hash, pcq, next_pcq) {
		free(pcq);
	}
	*hash = NULL;
}


/**
* Frees a player quest list.
*
* @param struct player_quest *list The list of player quests to free.
*/
void free_player_quests(struct player_quest *list) {
	struct player_quest *pq;
	while ((pq = list)) {
		list = pq->next;
		free_quest_tasks(pq->tracker);
		free(pq);
	}
}


/**
* @param struct quest_giver *list The list to free.
*/
void free_quest_givers(struct quest_giver *list) {
	struct quest_giver *iter, *next_iter;
	LL_FOREACH_SAFE(list, iter, next_iter) {
		free(iter);
	}
}


/**
* @param struct quest_reward *list The list to free.
*/
void free_quest_rewards(struct quest_reward *list) {
	struct quest_reward *iter, *next_iter;
	LL_FOREACH_SAFE(list, iter, next_iter) {
		free(iter);
	}
}


/**
* @param struct quest_task *list The list to free.
*/
void free_quest_tasks(struct quest_task *list) {
	struct quest_task *iter, *next_iter;
	LL_FOREACH_SAFE(list, iter, next_iter) {
		free(iter);
	}
}


/**
* Frees a temporary quest list.
*
* @param struct quest_temp_list *list The list to free.
*/
void free_quest_temp_list(struct quest_temp_list *list) {
	struct quest_temp_list *qtl, *next_qtl;
	LL_FOREACH_SAFE(list, qtl, next_qtl) {
		free(qtl);
	}
}


/**
* frees up memory for a quest data item.
*
* See also: olc_delete_quest
*
* @param quest_data *quest The quest data to free.
*/
void free_quest(quest_data *quest) {
	quest_data *proto = quest_proto(QUEST_VNUM(quest));
	
	// strings
	if (QUEST_NAME(quest) && (!proto || QUEST_NAME(quest) != QUEST_NAME(proto))) {
		free(QUEST_NAME(quest));
	}
	if (QUEST_DESCRIPTION(quest) && (!proto || QUEST_DESCRIPTION(quest) != QUEST_DESCRIPTION(proto))) {
		free(QUEST_DESCRIPTION(quest));
	}
	if (QUEST_COMPLETE_MSG(quest) && (!proto || QUEST_COMPLETE_MSG(quest) != QUEST_COMPLETE_MSG(proto))) {
		free(QUEST_COMPLETE_MSG(quest));
	}
	
	// pointers
	if (QUEST_STARTS_AT(quest) && (!proto || QUEST_STARTS_AT(quest) != QUEST_STARTS_AT(proto))) {
		free_quest_givers(QUEST_STARTS_AT(quest));
	}
	if (QUEST_ENDS_AT(quest) && (!proto || QUEST_ENDS_AT(quest) != QUEST_ENDS_AT(proto))) {
		free_quest_givers(QUEST_ENDS_AT(quest));
	}
	if (QUEST_TASKS(quest) && (!proto || QUEST_TASKS(quest) != QUEST_TASKS(proto))) {
		free_quest_tasks(QUEST_TASKS(quest));
	}
	if (QUEST_REWARDS(quest) && (!proto || QUEST_REWARDS(quest) != QUEST_REWARDS(proto))) {
		free_quest_rewards(QUEST_REWARDS(quest));
	}
	if (QUEST_PREREQS(quest) && (!proto || QUEST_PREREQS(quest) != QUEST_PREREQS(proto))) {
		free_quest_tasks(QUEST_PREREQS(quest));
	}
	if (QUEST_SCRIPTS(quest) && (!proto || QUEST_SCRIPTS(quest) != QUEST_SCRIPTS(proto))) {
		free_proto_scripts(&QUEST_SCRIPTS(quest));
	}
	
	free(quest);
}


/**
* Parses a quest giver, saved as:
*
* A
* 1 123
*
* @param FILE *fl The file, having just read the letter tag.
* @param struct quest_giver **list The list to append to.
* @param char *error_str How to report if there is an error.
*/
void parse_quest_giver(FILE *fl, struct quest_giver **list, char *error_str) {
	struct quest_giver *giver;
	char line[256];
	any_vnum vnum;
	int type;
	
	if (!fl || !list || !get_line(fl, line)) {
		log("SYSERR: data error in quest giver line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	if (sscanf(line, "%d %d", &type, &vnum) != 2) {
		log("SYSERR: format error in quest giver line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	
	CREATE(giver, struct quest_giver, 1);
	giver->type = type;
	giver->vnum = vnum;
	
	LL_APPEND(*list, giver);
}


/**
* Parses a quest reward, saved as:
*
* A
* 1 123 2
*
* @param FILE *fl The file, having just read the letter tag.
* @param struct quest_reward **list The list to append to.
* @param char *error_str How to report if there is an error.
*/
void parse_quest_reward(FILE *fl, struct quest_reward **list, char *error_str) {
	struct quest_reward *reward;
	int type, amount;
	char line[256];
	any_vnum vnum;
	
	if (!fl || !list || !get_line(fl, line)) {
		log("SYSERR: data error in quest reward line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	if (sscanf(line, "%d %d %d", &type, &vnum, &amount) != 3) {
		log("SYSERR: format error in quest reward line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	
	CREATE(reward, struct quest_reward, 1);
	reward->type = type;
	reward->vnum = vnum;
	reward->amount = amount;
	
	LL_APPEND(*list, reward);
}


/**
* Parses a quest task, saved as:
*
* A
* 1 123 123456 10
*
* @param FILE *fl The file, having just read the letter tag.
* @param struct quest_task **list The list to append to.
* @param char *error_str How to report if there is an error.
*/
void parse_quest_task(FILE *fl, struct quest_task **list, char *error_str) {
	struct quest_task *task;
	int type, needed;
	bitvector_t misc;
	char line[256];
	any_vnum vnum;
	
	if (!fl || !list || !get_line(fl, line)) {
		log("SYSERR: data error in quest task line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	if (sscanf(line, "%d %d %llu %d", &type, &vnum, &misc, &needed) != 4) {
		log("SYSERR: format error in quest task line of %s", error_str ? error_str : "UNKNOWN");
		exit(1);
	}
	
	CREATE(task, struct quest_task, 1);
	task->type = type;
	task->vnum = vnum;
	task->misc = misc;
	task->needed = needed;
	task->current = 0;
	
	LL_APPEND(*list, task);
}


/**
* Read one quest from file.
*
* @param FILE *fl The open .qst file
* @param any_vnum vnum The quest vnum
*/
void parse_quest(FILE *fl, any_vnum vnum) {
	char line[256], error[256], str_in[256];
	quest_data *quest, *find;
	int int_in[4];
	
	CREATE(quest, quest_data, 1);
	clear_quest(quest);
	QUEST_VNUM(quest) = vnum;
	
	HASH_FIND_INT(quest_table, &vnum, find);
	if (find) {
		log("WARNING: Duplicate quest vnum #%d", vnum);
		// but have to load it anyway to advance the file
	}
	add_quest_to_table(quest);
		
	// for error messages
	sprintf(error, "quest vnum %d", vnum);
	
	// lines 1-3: strings
	QUEST_NAME(quest) = fread_string(fl, error);
	QUEST_DESCRIPTION(quest) = fread_string(fl, error);
	QUEST_COMPLETE_MSG(quest) = fread_string(fl, error);
	
	// 4. version flags min max repeatable-after
	if (!get_line(fl, line) || sscanf(line, "%d %s %d %d %d", &int_in[0], str_in, &int_in[1], &int_in[2], &int_in[3]) != 5) {
		log("SYSERR: Format error in line 4 of %s", error);
		exit(1);
	}
	
	QUEST_VERSION(quest) = int_in[0];
	QUEST_FLAGS(quest) = asciiflag_conv(str_in);
	QUEST_MIN_LEVEL(quest) = int_in[1];
	QUEST_MAX_LEVEL(quest) = int_in[2];
	QUEST_REPEATABLE_AFTER(quest) = int_in[3];
	
	// optionals
	for (;;) {
		if (!get_line(fl, line)) {
			log("SYSERR: Format error in %s, expecting alphabetic flags", error);
			exit(1);
		}
		switch (*line) {
			case 'A': {	// starts at
				parse_quest_giver(fl, &QUEST_STARTS_AT(quest), error);
				break;
			}
			case 'P': {	// preq-requisites
				parse_quest_task(fl, &QUEST_PREREQS(quest), error);
				break;
			}
			case 'R': {	// rewards
				parse_quest_reward(fl, &QUEST_REWARDS(quest), error);
				break;
			}
			case 'T': {	// triggers
				parse_trig_proto(line, &QUEST_SCRIPTS(quest), error);
				break;
			}
			case 'W': {	// tasks / work
				parse_quest_task(fl, &QUEST_TASKS(quest), error);
				break;
			}
			case 'Z': {	// ends at
				parse_quest_giver(fl, &QUEST_ENDS_AT(quest), error);
				break;
			}
			
			// end
			case 'S': {
				return;
			}
			
			default: {
				log("SYSERR: Format error in %s, expecting alphabetic flags", error);
				exit(1);
			}
		}
	}
}


// writes entries in the quest index
void write_quest_index(FILE *fl) {
	quest_data *quest, *next_quest;
	int this, last;
	
	last = -1;
	HASH_ITER(hh, quest_table, quest, next_quest) {
		// determine "zone number" by vnum
		this = (int)(QUEST_VNUM(quest) / 100);
	
		if (this != last) {
			fprintf(fl, "%d%s\n", this, QST_SUFFIX);
			last = this;
		}
	}
}


/**
* Writes a list of 'quest_giver' to a data file.
*
* @param FILE *fl The file, open for writing.
* @param char letter The tag letter.
* @param struct quest_giver *list The list to write.
*/
void write_quest_givers_to_file(FILE *fl, char letter, struct quest_giver *list) {
	struct quest_giver *iter;
	LL_FOREACH(list, iter) {
		fprintf(fl, "%c\n%d %d\n", letter, iter->type, iter->vnum);
	}
}


/**
* Writes a list of 'quest_reward' to a data file.
*
* @param FILE *fl The file, open for writing.
* @param char letter The tag letter.
* @param struct quest_reward *list The list to write.
*/
void write_quest_rewards_to_file(FILE *fl, char letter, struct quest_reward *list) {
	struct quest_reward *iter;
	LL_FOREACH(list, iter) {
		fprintf(fl, "%c\n%d %d %d\n", letter, iter->type, iter->vnum, iter->amount);
	}
}


/**
* Writes a list of 'quest_task' to a data file.
*
* @param FILE *fl The file, open for writing.
* @param char letter The tag letter.
* @param struct quest_task *list The list to write.
*/
void write_quest_tasks_to_file(FILE *fl, char letter, struct quest_task *list) {
	struct quest_task *iter;
	LL_FOREACH(list, iter) {
		// NOTE: iter->current is NOT written to file
		fprintf(fl, "%c\n%d %d %llu %d\n", letter, iter->type, iter->vnum, iter->misc, iter->needed);
	}
}


/**
* Outputs one quest item in the db file format, starting with a #VNUM and
* ending with an S.
*
* @param FILE *fl The file to write it to.
* @param quest_data *quest The thing to save.
*/
void write_quest_to_file(FILE *fl, quest_data *quest) {
	void write_trig_protos_to_file(FILE *fl, char letter, struct trig_proto_list *list);
	
	char temp[MAX_STRING_LENGTH];
	
	if (!fl || !quest) {
		syslog(SYS_ERROR, LVL_START_IMM, TRUE, "SYSERR: write_quest_to_file called without %s", !fl ? "file" : "quest");
		return;
	}
	
	fprintf(fl, "#%d\n", QUEST_VNUM(quest));
	
	// 1. name
	fprintf(fl, "%s~\n", NULLSAFE(QUEST_NAME(quest)));
	
	// 2. desc
	strcpy(temp, NULLSAFE(QUEST_DESCRIPTION(quest)));
	strip_crlf(temp);
	fprintf(fl, "%s~\n", temp);
	
	// 3. complete msg
	strcpy(temp, NULLSAFE(QUEST_COMPLETE_MSG(quest)));
	strip_crlf(temp);
	fprintf(fl, "%s~\n", temp);
	
	// 4. version flags min max repeatable-after
	fprintf(fl, "%d %s %d %d %d\n", QUEST_VERSION(quest), bitv_to_alpha(QUEST_FLAGS(quest)), QUEST_MIN_LEVEL(quest), QUEST_MAX_LEVEL(quest), QUEST_REPEATABLE_AFTER(quest));
		
	// A. starts at
	write_quest_givers_to_file(fl, 'A', QUEST_STARTS_AT(quest));
	
	// P. pre-requisites
	write_quest_tasks_to_file(fl, 'P', QUEST_PREREQS(quest));
	
	// R. rewards
	write_quest_rewards_to_file(fl, 'R', QUEST_REWARDS(quest));
	
	// T. triggers
	write_trig_protos_to_file(fl, 'T', QUEST_SCRIPTS(quest));
	
	// W. tasks (work)
	write_quest_tasks_to_file(fl, 'W', QUEST_TASKS(quest));
	
	// Z. ends at
	write_quest_givers_to_file(fl, 'Z', QUEST_ENDS_AT(quest));
	
	// end
	fprintf(fl, "S\n");
}


 //////////////////////////////////////////////////////////////////////////////
//// OLC HANDLERS ////////////////////////////////////////////////////////////


/**
* Creates a new quest entry.
* 
* @param any_vnum vnum The number to create.
* @return quest_data* The new quest's prototype.
*/
quest_data *create_quest_table_entry(any_vnum vnum) {
	quest_data *quest;
	
	// sanity
	if (quest_proto(vnum)) {
		log("SYSERR: Attempting to insert quest at existing vnum %d", vnum);
		return quest_proto(vnum);
	}
	
	CREATE(quest, quest_data, 1);
	clear_quest(quest);
	QUEST_VNUM(quest) = vnum;
	QUEST_NAME(quest) = str_dup(default_quest_name);
	QUEST_DESCRIPTION(quest) = str_dup(default_quest_description);
	QUEST_COMPLETE_MSG(quest) = str_dup(default_quest_complete_msg);
	QUEST_FLAGS(quest) = QST_IN_DEVELOPMENT;
	add_quest_to_table(quest);

	// save index and quest file now
	save_index(DB_BOOT_QST);
	save_library_file_for_vnum(DB_BOOT_QST, vnum);

	return quest;
}


/**
* WARNING: This function actually deletes a quest.
*
* @param char_data *ch The person doing the deleting.
* @param any_vnum vnum The vnum to delete.
*/
void olc_delete_quest(char_data *ch, any_vnum vnum) {
	quest_data *quest, *qiter, *next_qiter;
	descriptor_data *desc;
	bool found;
	
	if (!(quest = quest_proto(vnum))) {
		msg_to_char(ch, "There is no such quest %d.\r\n", vnum);
		return;
	}
	
	// remove it from the hash table first
	remove_quest_from_table(quest);
	
	// look for people on the quest and force a refresh
	LL_FOREACH(descriptor_list, desc) {
		if (STATE(desc) != CON_PLAYING || !desc->character) {
			continue;
		}
		if (!is_on_quest(desc->character, vnum)) {
			continue;
		}
		refresh_all_quests(desc->character);
	}
	
	// save index and quest file now
	save_index(DB_BOOT_QST);
	save_library_file_for_vnum(DB_BOOT_QST, vnum);
	
	// delete from lookups
	add_or_remove_all_quest_lookups_for(quest, FALSE);
	
	// update other quests
	HASH_ITER(hh, quest_table, qiter, next_qiter) {
		// QT_x, QR_x: quest types
		found = delete_quest_task_from_list(&QUEST_TASKS(qiter), QT_COMPLETED_QUEST, vnum);
		found |= delete_quest_task_from_list(&QUEST_PREREQS(qiter), QT_COMPLETED_QUEST, vnum);
		found |= delete_quest_task_from_list(&QUEST_TASKS(qiter), QT_NOT_COMPLETED_QUEST, vnum);
		found |= delete_quest_task_from_list(&QUEST_PREREQS(qiter), QT_NOT_COMPLETED_QUEST, vnum);
		found |= delete_quest_task_from_list(&QUEST_TASKS(qiter), QT_NOT_ON_QUEST, vnum);
		found |= delete_quest_task_from_list(&QUEST_PREREQS(qiter), QT_NOT_ON_QUEST, vnum);
		found |= delete_quest_reward_from_list(&QUEST_REWARDS(qiter), QR_QUEST_CHAIN, vnum);
		
		if (found) {
			SET_BIT(QUEST_FLAGS(qiter), QST_IN_DEVELOPMENT);
			save_library_file_for_vnum(DB_BOOT_QST, QUEST_VNUM(qiter));
		}
	}
	
	// remove from from active editors
	for (desc = descriptor_list; desc; desc = desc->next) {
		if (GET_OLC_QUEST(desc)) {
			// QT_x, QR_x: quest types
			found = delete_quest_task_from_list(&QUEST_TASKS(GET_OLC_QUEST(desc)), QT_COMPLETED_QUEST, vnum);
			found |= delete_quest_task_from_list(&QUEST_PREREQS(GET_OLC_QUEST(desc)), QT_COMPLETED_QUEST, vnum);
			found |= delete_quest_task_from_list(&QUEST_TASKS(GET_OLC_QUEST(desc)), QT_NOT_COMPLETED_QUEST, vnum);
			found |= delete_quest_task_from_list(&QUEST_PREREQS(GET_OLC_QUEST(desc)), QT_NOT_COMPLETED_QUEST, vnum);
			found |= delete_quest_task_from_list(&QUEST_TASKS(GET_OLC_QUEST(desc)), QT_NOT_ON_QUEST, vnum);
			found |= delete_quest_task_from_list(&QUEST_PREREQS(GET_OLC_QUEST(desc)), QT_NOT_ON_QUEST, vnum);
			found |= delete_quest_reward_from_list(&QUEST_REWARDS(GET_OLC_QUEST(desc)), QR_QUEST_CHAIN, vnum);
		
			if (found) {
				SET_BIT(QUEST_FLAGS(GET_OLC_QUEST(desc)), QST_IN_DEVELOPMENT);
				msg_to_desc(desc, "Another quest used by the quest you are editing was deleted.\r\n");
			}
		}
	}
	
	syslog(SYS_OLC, GET_INVIS_LEV(ch), TRUE, "OLC: %s has deleted quest %d", GET_NAME(ch), vnum);
	msg_to_char(ch, "Quest %d deleted.\r\n", vnum);
	
	free_quest(quest);
}


/**
* Function to save a player's changes to a quest (or a new one).
*
* @param descriptor_data *desc The descriptor who is saving.
*/
void save_olc_quest(descriptor_data *desc) {	
	quest_data *proto, *quest = GET_OLC_QUEST(desc);
	any_vnum vnum = GET_OLC_VNUM(desc);
	descriptor_data *iter;
	UT_hash_handle hh;

	// have a place to save it?
	if (!(proto = quest_proto(vnum))) {
		proto = create_quest_table_entry(vnum);
	}
	
	// free prototype strings and pointers
	if (QUEST_NAME(proto)) {
		free(QUEST_NAME(proto));
	}
	if (QUEST_DESCRIPTION(proto)) {
		free(QUEST_DESCRIPTION(proto));
	}
	if (QUEST_COMPLETE_MSG(proto)) {
		free(QUEST_COMPLETE_MSG(proto));
	}
	free_quest_givers(QUEST_STARTS_AT(proto));
	free_quest_givers(QUEST_ENDS_AT(proto));
	free_quest_tasks(QUEST_TASKS(proto));
	free_quest_rewards(QUEST_REWARDS(proto));
	free_quest_tasks(QUEST_PREREQS(proto));
	free_proto_scripts(&QUEST_SCRIPTS(proto));
	
	// sanity
	if (!QUEST_NAME(quest) || !*QUEST_NAME(quest)) {
		if (QUEST_NAME(quest)) {
			free(QUEST_NAME(quest));
		}
		QUEST_NAME(quest) = str_dup(default_quest_name);
	}
	if (!QUEST_DESCRIPTION(quest) || !*QUEST_DESCRIPTION(quest)) {
		if (QUEST_DESCRIPTION(quest)) {
			free(QUEST_DESCRIPTION(quest));
		}
		QUEST_DESCRIPTION(quest) = str_dup(default_quest_description);
	}
	if (!QUEST_COMPLETE_MSG(quest) || !*QUEST_COMPLETE_MSG(quest)) {
		if (QUEST_COMPLETE_MSG(quest)) {
			free(QUEST_COMPLETE_MSG(quest));
		}
		QUEST_COMPLETE_MSG(quest) = str_dup(default_quest_complete_msg);
	}
	
	// delete from lookups
	add_or_remove_all_quest_lookups_for(proto, FALSE);
	
	// save data back over the proto-type
	hh = proto->hh;	// save old hash handle
	*proto = *quest;	// copy over all data
	proto->vnum = vnum;	// ensure correct vnum
	proto->hh = hh;	// restore old hash handle
	
	// re-add lookups
	add_or_remove_all_quest_lookups_for(proto, TRUE);
	
	// and save to file
	save_library_file_for_vnum(DB_BOOT_QST, vnum);
	
	// look for players on the quest and update them
	LL_FOREACH(descriptor_list, iter) {
		if (STATE(iter) != CON_PLAYING || !iter->character) {
			continue;
		}
		if (!is_on_quest(iter->character, vnum)) {
			continue;
		}
		refresh_all_quests(iter->character);
	}
}


/**
* Creates a copy of a quest, or clears a new one, for editing.
* 
* @param quest_data *input The quest to copy, or NULL to make a new one.
* @return quest_data* The copied quest.
*/
quest_data *setup_olc_quest(quest_data *input) {
	extern struct apply_data *copy_apply_list(struct apply_data *input);
	
	quest_data *new;
	
	CREATE(new, quest_data, 1);
	clear_quest(new);
	
	if (input) {
		// copy normal data
		*new = *input;

		// copy things that are pointers
		QUEST_NAME(new) = QUEST_NAME(input) ? str_dup(QUEST_NAME(input)) : NULL;
		QUEST_DESCRIPTION(new) = QUEST_DESCRIPTION(input) ? str_dup(QUEST_DESCRIPTION(input)) : NULL;
		QUEST_COMPLETE_MSG(new) = QUEST_COMPLETE_MSG(input) ? str_dup(QUEST_COMPLETE_MSG(input)) : NULL;
		
		QUEST_STARTS_AT(new) = copy_quest_givers(QUEST_STARTS_AT(input));
		QUEST_ENDS_AT(new) = copy_quest_givers(QUEST_ENDS_AT(input));
		QUEST_TASKS(new) = copy_quest_tasks(QUEST_TASKS(input));
		QUEST_REWARDS(new) = copy_quest_rewards(QUEST_REWARDS(input));
		QUEST_PREREQS(new) = copy_quest_tasks(QUEST_PREREQS(input));
		QUEST_SCRIPTS(new) = copy_trig_protos(QUEST_SCRIPTS(input));
		
		// update version number
		QUEST_VERSION(new) += 1;
	}
	else {
		// brand new: some defaults
		QUEST_NAME(new) = str_dup(default_quest_name);
		QUEST_DESCRIPTION(new) = str_dup(default_quest_description);
		QUEST_COMPLETE_MSG(new) = str_dup(default_quest_complete_msg);
		QUEST_FLAGS(new) = QST_IN_DEVELOPMENT;
		QUEST_VERSION(new) = 1;
	}
	
	// done
	return new;	
}


 //////////////////////////////////////////////////////////////////////////////
//// DISPLAYS ////////////////////////////////////////////////////////////////

/**
* Gets the display for a set of quest givers.
*
* @param struct quest_giver *list Pointer to the start of a list of quest givers.
* @param char *save_buffer A buffer to store the result to.
*/
void get_quest_giver_display(struct quest_giver *list, char *save_buffer) {
	struct quest_giver *giver;
	int count = 0;
	
	*save_buffer = '\0';
	LL_FOREACH(list, giver) {		
		sprintf(save_buffer + strlen(save_buffer), "%2d. %s\r\n", ++count, quest_giver_string(giver, TRUE));
	}
	
	// empty list not shown
}


/**
* Gets the display for a set of quest rewards.
*
* @param struct quest_reward *list Pointer to the start of a list of quest rewards.
* @param char *save_buffer A buffer to store the result to.
*/
void get_quest_reward_display(struct quest_reward *list, char *save_buffer) {
	struct quest_reward *reward;
	int count = 0;
	
	*save_buffer = '\0';
	LL_FOREACH(list, reward) {		
		sprintf(save_buffer + strlen(save_buffer), "%2d. %s: %s\r\n", ++count, quest_reward_types[reward->type], quest_reward_string(reward, TRUE));
	}
	
	// empty list not shown
}


/**
* Gets the display for a set of quest tasks.
*
* @param struct quest_task *list Pointer to the start of a list of quest tasks.
* @param char *save_buffer A buffer to store the result to.
*/
void get_quest_task_display(struct quest_task *list, char *save_buffer) {
	struct quest_task *task;
	int count = 0;
	
	*save_buffer = '\0';
	LL_FOREACH(list, task) {
		sprintf(save_buffer + strlen(save_buffer), "%2d. %s: %s\r\n", ++count, quest_tracker_types[task->type], quest_task_string(task, TRUE));
	}
	
	// empty list not shown
}

/**
* For vstat.
*
* @param char_data *ch The player requesting stats.
* @param quest_data *quest The quest to display.
*/
void do_stat_quest(char_data *ch, quest_data *quest) {
	char buf[MAX_STRING_LENGTH], part[MAX_STRING_LENGTH];
	size_t size;
	
	if (!quest) {
		return;
	}
	
	// first line
	size = snprintf(buf, sizeof(buf), "VNum: [\tc%d\t0], Name: \tc%s\t0\r\n", QUEST_VNUM(quest), QUEST_NAME(quest));
	size += snprintf(buf + size, sizeof(buf) - size, "%s", QUEST_DESCRIPTION(quest));
	size += snprintf(buf + size, sizeof(buf) - size, "-------------------------------------------------\r\n");
	size += snprintf(buf + size, sizeof(buf) - size, "%s", QUEST_COMPLETE_MSG(quest));
	
	sprintbit(QUEST_FLAGS(quest), quest_flags, part, TRUE);
	size += snprintf(buf + size, sizeof(buf) - size, "Flags: \tg%s\t0\r\n", part);
	
	if (QUEST_REPEATABLE_AFTER(quest) == NOT_REPEATABLE) {
		strcpy(part, "never");
	}
	else if (QUEST_REPEATABLE_AFTER(quest) == 0) {
		strcpy(part, "immediate");
	}
	else {
		sprintf(part, "%d minutes (%d:%02d:%02d)", QUEST_REPEATABLE_AFTER(quest), (QUEST_REPEATABLE_AFTER(quest) / (60 * 24)), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) / 60), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) % 60));
	}
	size += snprintf(buf + size, sizeof(buf) - size, "Level limits: [\tc%s\t0], Repeatable: [\tc%s\t0]\r\n", level_range_string(QUEST_MIN_LEVEL(quest), QUEST_MAX_LEVEL(quest), 0), part);
		
	get_quest_task_display(QUEST_PREREQS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Pre-requisites:\r\n%s", *part ? part : " none\r\n");
	
	get_quest_giver_display(QUEST_STARTS_AT(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Starts at:\r\n%s", *part ? part : " nowhere\r\n");
	
	get_quest_giver_display(QUEST_ENDS_AT(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Ends at:\r\n%s", *part ? part : " nowhere\r\n");
	
	get_quest_task_display(QUEST_TASKS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Tasks:\r\n%s", *part ? part : " none\r\n");
	
	get_quest_reward_display(QUEST_REWARDS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Rewards:\r\n%s", *part ? part : " none\r\n");
	
	// scripts
	get_script_display(QUEST_SCRIPTS(quest), part);
	size += snprintf(buf + size, sizeof(buf) - size, "Scripts:\r\n%s", QUEST_SCRIPTS(quest) ? part : " none\r\n");
	
	page_string(ch->desc, buf, TRUE);
}


/**
* This is the main recipe display for quest OLC. It displays the user's
* currently-edited quest.
*
* @param char_data *ch The person who is editing a quest and will see its display.
*/
void olc_show_quest(char_data *ch) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	char buf[MAX_STRING_LENGTH], lbuf[MAX_STRING_LENGTH];
	
	if (!quest) {
		return;
	}
	
	*buf = '\0';
	
	sprintf(buf + strlen(buf), "[\tc%d\t0] \tc%s\t0\r\n", GET_OLC_VNUM(ch->desc), !quest_proto(QUEST_VNUM(quest)) ? "new quest" : get_quest_name_by_proto(QUEST_VNUM(quest)));
	sprintf(buf + strlen(buf), "<\tyname\t0> %s\r\n", NULLSAFE(QUEST_NAME(quest)));
	sprintf(buf + strlen(buf), "<\tydescription\t0>\r\n%s", NULLSAFE(QUEST_DESCRIPTION(quest)));
	sprintf(buf + strlen(buf), "<\tycompletemessage\t0>\r\n%s", NULLSAFE(QUEST_COMPLETE_MSG(quest)));
	
	sprintbit(QUEST_FLAGS(quest), quest_flags, lbuf, TRUE);
	sprintf(buf + strlen(buf), "<\tyflags\t0> %s\r\n", lbuf);
	
	if (QUEST_MIN_LEVEL(quest) > 0) {
		sprintf(buf + strlen(buf), "<\tyminlevel\t0> %d\r\n", QUEST_MIN_LEVEL(quest));
	}
	else {
		sprintf(buf + strlen(buf), "<\tyminlevel\t0> none\r\n");
	}
	if (QUEST_MAX_LEVEL(quest) > 0) {
		sprintf(buf + strlen(buf), "<\tymaxlevel\t0> %d\r\n", QUEST_MAX_LEVEL(quest));
	}
	else {
		sprintf(buf + strlen(buf), "<\tymaxlevel\t0> none\r\n");
	}
	
	get_quest_task_display(QUEST_PREREQS(quest), lbuf);
	sprintf(buf + strlen(buf), "Pre-requisites: <\typrereqs\t0>\r\n%s", lbuf);
	
	if (QUEST_REPEATABLE_AFTER(quest) == NOT_REPEATABLE) {
		sprintf(buf + strlen(buf), "<\tyrepeat\t0> never\r\n");
	}
	else if (QUEST_REPEATABLE_AFTER(quest) > 0) {
		sprintf(buf + strlen(buf), "<\tyrepeat\t0> %d minutes (%d:%02d:%02d)\r\n", QUEST_REPEATABLE_AFTER(quest), (QUEST_REPEATABLE_AFTER(quest) / (60 * 24)), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) / 60), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) % 60));
	}
	else if (QUEST_REPEATABLE_AFTER(quest) == 0) {
		sprintf(buf + strlen(buf), "<\tyrepeat\t0> immediately\r\n");
	}
	
	get_quest_giver_display(QUEST_STARTS_AT(quest), lbuf);
	sprintf(buf + strlen(buf), "Starts at: <\tystarts\t0>\r\n%s", lbuf);
	
	get_quest_giver_display(QUEST_ENDS_AT(quest), lbuf);
	sprintf(buf + strlen(buf), "Ends at: <\tyends\t0>\r\n%s", lbuf);
	
	get_quest_task_display(QUEST_TASKS(quest), lbuf);
	sprintf(buf + strlen(buf), "Tasks: <\tytasks\t0>\r\n%s", lbuf);
	
	get_quest_reward_display(QUEST_REWARDS(quest), lbuf);
	sprintf(buf + strlen(buf), "Rewards: <\tyrewards\t0>\r\n%s", lbuf);
	
	// scripts
	sprintf(buf + strlen(buf), "Scripts: <\tyscript\t0>\r\n");
	if (QUEST_SCRIPTS(quest)) {
		get_script_display(QUEST_SCRIPTS(quest), lbuf);
		strcat(buf, lbuf);
	}
	
	page_string(ch->desc, buf, TRUE);
}


/**
* Searches the quest db for a match, and prints it to the character.
*
* @param char *searchname The search string.
* @param char_data *ch The player who is searching.
* @return int The number of matches shown.
*/
int vnum_quest(char *searchname, char_data *ch) {
	quest_data *iter, *next_iter;
	int found = 0;
	
	HASH_ITER(hh, quest_table, iter, next_iter) {
		if (multi_isname(searchname, QUEST_NAME(iter))) {
			msg_to_char(ch, "%3d. [%5d] %s\r\n", ++found, QUEST_VNUM(iter), QUEST_NAME(iter));
		}
	}
	
	return found;
}


 //////////////////////////////////////////////////////////////////////////////
//// OLC MODULES /////////////////////////////////////////////////////////////

OLC_MODULE(qedit_completemessage) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	
	if (ch->desc->str) {
		msg_to_char(ch, "You are already editing a string.\r\n");
	}
	else {
		sprintf(buf, "completion message for %s", QUEST_NAME(quest));
		start_string_editor(ch->desc, buf, &QUEST_COMPLETE_MSG(quest), MAX_ITEM_DESCRIPTION);
	}
}

OLC_MODULE(qedit_description) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	
	if (ch->desc->str) {
		msg_to_char(ch, "You are already editing a string.\r\n");
	}
	else {
		sprintf(buf, "description for %s", QUEST_NAME(quest));
		start_string_editor(ch->desc, buf, &QUEST_DESCRIPTION(quest), MAX_ITEM_DESCRIPTION);
	}
}


OLC_MODULE(qedit_ends) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	qedit_process_quest_givers(ch, argument, &QUEST_ENDS_AT(quest), "ends");
}


OLC_MODULE(qedit_flags) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	bool had_indev = IS_SET(QUEST_FLAGS(quest), QST_IN_DEVELOPMENT) ? TRUE : FALSE;
	
	QUEST_FLAGS(quest) = olc_process_flag(ch, argument, "quest", "flags", quest_flags, QUEST_FLAGS(quest));
	
	// validate removal of IN-DEVELOPMENT
	if (had_indev && !IS_SET(QUEST_FLAGS(quest), QST_IN_DEVELOPMENT) && GET_ACCESS_LEVEL(ch) < LVL_UNRESTRICTED_BUILDER && !OLC_FLAGGED(ch, OLC_FLAG_CLEAR_IN_DEV)) {
		msg_to_char(ch, "You don't have permission to remove the IN-DEVELOPMENT flag.\r\n");
		SET_BIT(QUEST_FLAGS(quest), QST_IN_DEVELOPMENT);
	}
}


OLC_MODULE(qedit_name) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	olc_process_string(ch, argument, "name", &QUEST_NAME(quest));
}


OLC_MODULE(qedit_maxlevel) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	QUEST_MAX_LEVEL(quest) = olc_process_number(ch, argument, "maximum level", "maxlevel", 0, MAX_INT, QUEST_MAX_LEVEL(quest));
}


OLC_MODULE(qedit_minlevel) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	QUEST_MIN_LEVEL(quest) = olc_process_number(ch, argument, "minimum level", "minlevel", 0, MAX_INT, QUEST_MIN_LEVEL(quest));
}


OLC_MODULE(qedit_prereqs) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	qedit_process_quest_tasks(ch, argument, &QUEST_PREREQS(quest), "prereq");
}


OLC_MODULE(qedit_repeat) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	
	if (!*argument) {
		msg_to_char(ch, "Set the repeat interval to how many minutes (or immediately/never)?\r\n");
	}
	else if (is_abbrev(argument, "never") || is_abbrev(argument, "none")) {
		QUEST_REPEATABLE_AFTER(quest) = NOT_REPEATABLE;
		msg_to_char(ch, "It is now non-repeatable.\r\n");
	}
	else if (is_abbrev(argument, "immediately")) {
		QUEST_REPEATABLE_AFTER(quest) = 0;
		msg_to_char(ch, "It is now immediately repeatable.\r\n");
	}
	else if (isdigit(*argument)) {
		QUEST_REPEATABLE_AFTER(quest) = olc_process_number(ch, argument, "repeatable after", "repeat", 0, MAX_INT, QUEST_REPEATABLE_AFTER(quest));
		msg_to_char(ch, "It now repeats after %d minutes (%d:%02d:%02d).\r\n", QUEST_REPEATABLE_AFTER(quest), (QUEST_REPEATABLE_AFTER(quest) / (60 * 24)), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) / 60), ((QUEST_REPEATABLE_AFTER(quest) % (60 * 24)) % 60));
	}
	else {
		msg_to_char(ch, "Invalid repeat interval.\r\n");
	}
}


OLC_MODULE(qedit_rewards) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	char cmd_arg[MAX_INPUT_LENGTH], field_arg[MAX_INPUT_LENGTH];
	char num_arg[MAX_INPUT_LENGTH], type_arg[MAX_INPUT_LENGTH];
	char vnum_arg[MAX_INPUT_LENGTH], buf[MAX_STRING_LENGTH];
	struct quest_reward *reward, *iter, *change, *copyfrom;
	struct quest_reward **list = &QUEST_REWARDS(quest);
	int findtype, num, stype;
	bool found, ok;
	any_vnum vnum;
	
	argument = any_one_arg(argument, cmd_arg);	// add/remove/change/copy
	
	if (is_abbrev(cmd_arg, "copy")) {
		// usage: rewards copy <from type> <from vnum>
		argument = any_one_arg(argument, type_arg);	// just "quest" for now
		argument = any_one_arg(argument, vnum_arg);	// any vnum for that type
		
		if (!*type_arg || !*vnum_arg) {
			msg_to_char(ch, "Usage: rewards copy <from type> <from vnum>\r\n");
		}
		else if ((findtype = find_olc_type(type_arg)) == 0) {
			msg_to_char(ch, "Unknown olc type '%s'.\r\n", type_arg);
		}
		else if (!isdigit(*vnum_arg)) {
			sprintbit(findtype, olc_type_bits, buf, FALSE);
			msg_to_char(ch, "Copy from which %s?\r\n", buf);
		}
		else if ((vnum = atoi(vnum_arg)) < 0) {
			msg_to_char(ch, "Invalid vnum.\r\n");
		}
		else {
			sprintbit(findtype, olc_type_bits, buf, FALSE);
			copyfrom = NULL;
			
			switch (findtype) {
				case OLC_QUEST: {
					quest_data *from_qst = quest_proto(vnum);
					if (from_qst) {
						copyfrom = QUEST_REWARDS(from_qst);
					}
					break;
				}
				default: {
					msg_to_char(ch, "You can't copy rewards from %ss.\r\n", buf);
					return;
				}
			}
			
			if (!copyfrom) {
				msg_to_char(ch, "Invalid %s vnum '%s'.\r\n", buf, vnum_arg);
			}
			else {
				smart_copy_quest_rewards(list, copyfrom);
				msg_to_char(ch, "Copied rewards from %s %d.\r\n", buf, vnum);
			}
		}
	}	// end 'copy'
	else if (is_abbrev(cmd_arg, "remove")) {
		// usage: rewards remove <number | all>
		skip_spaces(&argument);	// only arg is number
		
		if (!*argument) {
			msg_to_char(ch, "Remove which reward (number)?\r\n");
		}
		else if (!str_cmp(argument, "all")) {
			free_quest_rewards(*list);
			*list = NULL;
			msg_to_char(ch, "You remove all the rewards.\r\n");
		}
		else if (!isdigit(*argument) || (num = atoi(argument)) < 1) {
			msg_to_char(ch, "Invalid reward number.\r\n");
		}
		else {
			found = FALSE;
			LL_FOREACH(*list, iter) {
				if (--num == 0) {
					found = TRUE;
					
					msg_to_char(ch, "You remove the reward for %dx %s %d.\r\n", iter->amount, quest_reward_types[iter->type], iter->vnum);
					LL_DELETE(*list, iter);
					free(iter);
					break;
				}
			}
			
			if (!found) {
				msg_to_char(ch, "Invalid reward number.\r\n");
			}
		}
	}	// end 'remove'
	else if (is_abbrev(cmd_arg, "add")) {
		// usage: rewards add <type> <amount> <vnum/type>
		argument = any_one_arg(argument, type_arg);
		argument = any_one_arg(argument, num_arg);
		argument = any_one_arg(argument, vnum_arg);
		
		if (!*type_arg || !*num_arg || !isdigit(*num_arg)) {
			msg_to_char(ch, "Usage: rewards add <type> <amount> <vnum/type>\r\n");
		}
		else if ((stype = search_block(type_arg, quest_reward_types, FALSE)) == NOTHING) {
			msg_to_char(ch, "Invalid type '%s'.\r\n", type_arg);
		}
		else if ((num = atoi(num_arg)) < 1) {
			msg_to_char(ch, "Invalid amount '%s'.\r\n", num_arg);
		}
		else {		
			// QR_x: validate vnum
			vnum = 0;
			ok = FALSE;
			switch (stype) {
				case QR_BONUS_EXP: {
					// vnum not required
					ok = TRUE;
					break;
				}
				case QR_COINS: {
					if (is_abbrev(vnum_arg, "miscellaneous") || is_abbrev(vnum_arg, "simple") || is_abbrev(vnum_arg, "other")) {
						vnum = OTHER_COIN;
						ok = TRUE;
					}
					else if (is_abbrev(vnum_arg, "empire")) {
						vnum = REWARD_EMPIRE_COIN;
						ok = TRUE;
					}
					else {
						msg_to_char(ch, "You must choose misc or empire coins.\r\n");
						return;
					}
					break;	
				}
				case QR_OBJECT: {
					if (!*vnum_arg) {
						msg_to_char(ch, "Usage: rewards add object <amount> <object vnum>\r\n");
						return;
					}
					if (!isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
						msg_to_char(ch, "Invalid obj vnum '%s'.\r\n", vnum_arg);
						return;
					}
					if (obj_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QR_SET_SKILL:
				case QR_SKILL_EXP:
				case QR_SKILL_LEVELS: {
					if (!*vnum_arg) {
						msg_to_char(ch, "Usage: rewards add <set-skill | skill-exp | skill-levels> <level> <skill vnum>\r\n");
						return;
					}
					if (!isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
						msg_to_char(ch, "Invalid skill vnum '%s'.\r\n", vnum_arg);
						return;
					}
					if (find_skill_by_vnum(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QR_QUEST_CHAIN: {
					if (!*vnum_arg) {
						strcpy(vnum_arg, num_arg);	// they may have omitted amount
					}
					if (!*vnum_arg || !isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
						msg_to_char(ch, "Invalid quest vnum '%s'.\r\n", vnum_arg);
						return;
					}
					if (quest_proto(vnum)) {
						ok = TRUE;
					}
					// amount is not used here
					num = 1;
					break;
				}
			}
			
			// did we find one?
			if (!ok) {
				msg_to_char(ch, "Unable to find %s %d.\r\n", quest_reward_types[stype], vnum);
				return;
			}
			
			// success
			CREATE(reward, struct quest_reward, 1);
			reward->type = stype;
			reward->amount = num;
			reward->vnum = vnum;
			
			LL_APPEND(*list, reward);
			msg_to_char(ch, "You add a %s reward: %s\r\n", quest_reward_types[stype], quest_reward_string(reward, TRUE));
		}
	}	// end 'add'
	else if (is_abbrev(cmd_arg, "change")) {
		// usage: rewards change <number> <amount | vnum> <value>
		argument = any_one_arg(argument, num_arg);
		argument = any_one_arg(argument, field_arg);
		argument = any_one_arg(argument, vnum_arg);
		
		if (!*num_arg || !isdigit(*num_arg) || !*field_arg || !*vnum_arg) {
			msg_to_char(ch, "Usage: rewards change <number> <amount | vnum> <value>\r\n");
			return;
		}
		
		// find which one to change
		num = atoi(num_arg);
		change = NULL;
		LL_FOREACH(*list, iter) {
			if (--num == 0) {
				change = iter;
				break;
			}
		}
		
		if (!change) {
			msg_to_char(ch, "Invalid reward number.\r\n");
		}
		else if (is_abbrev(field_arg, "amount")) {
			if (!isdigit(*vnum_arg) || (num = atoi(vnum_arg)) < 0) {
				msg_to_char(ch, "Invalid amount '%s'.\r\n", vnum_arg);
				return;
			}
			else {
				change->amount = num;
				msg_to_char(ch, "You change reward %d to: %s\r\n", atoi(num_arg), quest_reward_string(change, TRUE));
			}
		}
		else if (is_abbrev(field_arg, "vnum")) {
			// QR_x: validate vnum
			vnum = 0;
			ok = FALSE;
			switch (change->type) {
				case QR_BONUS_EXP: {
					msg_to_char(ch, "You can't change the vnum on that.\r\n");
					break;
				}
				case QR_COINS: {
					if (is_abbrev(vnum_arg, "miscellaneous") || is_abbrev(vnum_arg, "simple") || is_abbrev(vnum_arg, "other")) {
						vnum = OTHER_COIN;
						ok = TRUE;
					}
					else if (is_abbrev(vnum_arg, "empire")) {
						vnum = REWARD_EMPIRE_COIN;
						ok = TRUE;
					}
					else {
						msg_to_char(ch, "You must choose misc or empire coins.\r\n");
						return;
					}
					break;	
				}
				case QR_OBJECT: {
					if (!*vnum_arg || !isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
						msg_to_char(ch, "Invalid obj vnum '%s'.\r\n", vnum_arg);
						return;
					}
					if (obj_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QR_SET_SKILL:
				case QR_SKILL_EXP:
				case QR_SKILL_LEVELS: {
					if (!*vnum_arg || !isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
						msg_to_char(ch, "Invalid skill vnum '%s'.\r\n", vnum_arg);
						return;
					}
					if (find_skill_by_vnum(vnum)) {
						ok = TRUE;
					}
					break;
				}
				case QR_QUEST_CHAIN: {
					if (!*vnum_arg || !isdigit(*vnum_arg) || (vnum = atoi(vnum_arg)) < 0) {
						msg_to_char(ch, "Invalid quest vnum '%s'.\r\n", vnum_arg);
						return;
					}
					if (quest_proto(vnum)) {
						ok = TRUE;
					}
					break;
				}
			}
			
			// did we find one?
			if (!ok) {
				msg_to_char(ch, "Unable to find %s %d.\r\n", quest_giver_types[change->type], vnum);
				return;
			}
			
			change->vnum = vnum;
			msg_to_char(ch, "Changed reward %d to: %s\r\n", atoi(num_arg), quest_reward_string(change, TRUE));
		}
		else {
			msg_to_char(ch, "You can only change the amount or vnum.\r\n");
		}
	}	// end 'change'
	else if (is_abbrev(cmd_arg, "move")) {
		struct quest_reward *to_move, *prev, *a, *b, *a_next, *b_next, iitem;
		bool up;
		
		// usage: rewards move <number> <up | down>
		argument = any_one_arg(argument, num_arg);
		argument = any_one_arg(argument, field_arg);
		up = is_abbrev(field_arg, "up");
		
		if (!*num_arg || !*field_arg) {
			msg_to_char(ch, "Usage: rewards move <number> <up | down>\r\n");
		}
		else if (!isdigit(*num_arg) || (num = atoi(num_arg)) < 1) {
			msg_to_char(ch, "Invalid reward number.\r\n");
		}
		else if (!is_abbrev(field_arg, "up") && !is_abbrev(field_arg, "down")) {
			msg_to_char(ch, "You must specify whether you're moving it up or down in the list.\r\n");
		}
		else if (up && num == 1) {
			msg_to_char(ch, "You can't move it up; it's already at the top of the list.\r\n");
		}
		else {
			// find the one to move
			to_move = prev = NULL;
			for (reward = *list; reward && !to_move; reward = reward->next) {
				if (--num == 0) {
					to_move = reward;
				}
				else {
					// store for next iteration
					prev = reward;
				}
			}
			
			if (!to_move) {
				msg_to_char(ch, "Invalid reward number.\r\n");
			}
			else if (!up && !to_move->next) {
				msg_to_char(ch, "You can't move it down; it's already at the bottom of the list.\r\n");
			}
			else {
				// SUCCESS: "move" them by swapping data
				if (up) {
					a = prev;
					b = to_move;
				}
				else {
					a = to_move;
					b = to_move->next;
				}
				
				// store next pointers
				a_next = a->next;
				b_next = b->next;
				
				// swap data
				iitem = *a;
				*a = *b;
				*b = iitem;
				
				// restore next pointers
				a->next = a_next;
				b->next = b_next;
				
				// message: re-atoi(num_arg) because we destroyed num finding our target
				msg_to_char(ch, "You move reward %d %s.\r\n", atoi(num_arg), (up ? "up" : "down"));
			}
		}
	}	// end 'move'
	else {
		msg_to_char(ch, "Usage: rewards add <type> <amount> <vnum/type>\r\n");
		msg_to_char(ch, "Usage: rewards change <number> vnum <value>\r\n");
		msg_to_char(ch, "Usage: rewards copy <from type> <from vnum>\r\n");
		msg_to_char(ch, "Usage: rewards remove <number | all>\r\n");
		msg_to_char(ch, "Usage: rewards move <number> <up | down>\r\n");
	}
}


OLC_MODULE(qedit_script) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	olc_process_script(ch, argument, &QUEST_SCRIPTS(quest), WLD_TRIGGER);
}


OLC_MODULE(qedit_starts) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	qedit_process_quest_givers(ch, argument, &QUEST_STARTS_AT(quest), "starts");
}


OLC_MODULE(qedit_tasks) {
	quest_data *quest = GET_OLC_QUEST(ch->desc);
	qedit_process_quest_tasks(ch, argument, &QUEST_TASKS(quest), "task");
}
