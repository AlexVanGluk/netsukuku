/* This file is part of Netsukuku system
 * (c) Copyright 2004 Andrea Lo Pumo aka AlpT <alpt@freaknet.org>
 *
 * This source code is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Public License as published 
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This source code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Please refer to the GNU Public License for more details.
 *
 * You should have received a copy of the GNU Public License along with
 * this source code; if not, write to:
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <sys/time.h>
#include <sys/types.h>
#include "pkts.h"

#undef  QSPN_EMPIRIC

#ifndef QSPN_EMPIRIC
	#define MAXGROUPNODE		0x281
	#define MAXROUTES	 	20
#else
	#define MAXGROUPNODE		20
	#define MAXROUTES	 	5
#endif /*QSPN_EMPIRIC*/

#define MAXLINKS		MAXROUTES
#define MAXRTT			10		/*Max node <--> node rtt (in sec)*/

/*****The real map stuff*****/
/***flags**/
#define MAP_ME		1		/*The root_node, in other words, me ;)*/
#define MAP_HNODE	(1<<1)		/*Hooking node. One node is hooking when it is connecting to netsukuku*/
#define MAP_SNODE	(1<<2)
#define MAP_DNODE	(1<<3)
#define MAP_BNODE	(1<<4)		/*The node is a border_node*/
#define MAP_GNODE	(1<<5)
#define MAP_RNODE	(1<<6)		/*If a node has this set, it is one of the rnodes*/
#define MAP_UPDATE	(1<<7)		/*If it is set, the corresponding route in the krnl will be updated*/
#define MAP_VOID	(1<<8)		/*It indicates a non existent node*/
#define QSPN_CLOSED	(1<<9)		/*This flag is set only to the rnodes, it puts a link in a QSPN_CLOSED state*/
#define QSPN_OPENED	(1<<10)		/*It puts a link in a QSPN_OPEN state*/
#define QSPN_REPLIED	(1<<12)		/*When the node send the qspn_reply it will never reply again to the same qspn*/
#define QSPN_BACKPRO	(1<<13)		/*This marks the r_node where the QSPN_BACKPRO has been sent*/
#define QSPN_OLD	(1<<14)		/*If a node isn't updated by the current qspn_round it is marked with QSPN_ROUND.
					  If in the next qspn_round the same node isn't updated it is removed from the map.*/
#ifdef QSPN_EMPIRIC
#define QSPN_STARTER	(1<<15)		/*Used only by qspn-empiric.c*/
#endif
/* 			    *** Map notes ***
 * The map is a block of MAXGROUPNODE map_node struct. It is a generic map and it is
 * used to keep the qspn_map, the internal map and the external map.
 * The position in the map of each struct corresponds to its relative ip. For
 * example, if the map goes from 192.128.1.0 to 192.128.3.0, the map will have 512
 * structs, the first one will correspond to 192.168.1.0, the 50th to 192.168.1.50 and
 * so on.
 */
/*map_rnode is what map_node.r_node points to*/
typedef struct
{
#ifdef QSPN_EMPIRIC
	u_short		flags;
#endif
	u_int	 	*r_node;		 /*It's the pointer to the struct of the r_node in the map*/
	struct timeval  rtt;	 		 /*node <-> r_node round trip time*/
	
	struct timeval  trtt;			/*node <-> root_node total rtt: The rtt to reach the root_node 
	 					  starting from the node which uses this rnode. 
	 * Cuz I've explained it in such a bad way I make an example:
	 * map_node node_A; From node_A "node_A.links"th routes to the root_node start. 
	 * So I have "node_A.links"th node_A.r_node[s], each of them is a different route to reach the root_node. 
	 * With the node_A.r_node[route_number_to_follow].trtt I can get the rtt needed to reach the root_node 
	 * starting from the node_A using the route_number_to_follow. Gotcha? I hope so.
	 * Note: The trtt is mainly used for the sort of the routes*/
}map_rnode;

typedef struct
{
	u_short 	flags;
#ifdef QSPN_EMPIRIC
	u_int		brdcast[MAXGROUPNODE];
#else
	u_int		brdcast;	 /*Pkt_id of the last brdcast_pkt sent by this node*/
#endif /*QSPN_EMPIRIC*/
	__u16		links;		 /*Number of r_nodes*/
	map_rnode	*r_node;	 /*This structs will be kept in ascending order considering their rnode_t.rtt*/
}map_node;

#define MAXRNODEBLOCK		MAXLINKS*MAXGROUPNODE*sizeof(map_rnode)
#define INTMAP_END(mapstart)	((sizeof(map_node)*MAXGROUPNODE)+(mapstart))

/* map_bnode is the struct used to create the "map boarder node". 
 * This map keeps for each boarder node of the int_map the gnodes which they are linked to.
 * As always there are some little differences from the map_node:
 *
 *	__u16 		links;		is the number of gnodes the bnode is linked to.
 *	map_rnode	*r_node;	r_node[x].r_node, in this case, points to the position of the bnode's gnode in 
 *					the ext_map.
 *	u_int           brdcast;	Where this node is in the int_map. The position is stored in the usual
 *					pos_from_node() format. (Yep, a dirty hack)
 *
 * So you are asking why didn't I made a new struct for the bmap. Well, I don't want to [re]write all the functions 
 * to handle the map, for example rnode_add,rnode_del, save_map, etc... it's a pain, just for a little map and moreover
 * it adds new potential bugs. In conclusion: laziness + fear == hacks++;
 */
#define bnode_ptr	brdcast		/*Don't kill me*/
typedef map_node map_bnode;

#define MAXGROUPBNODE		MAXGROUPNODE	/*the maximum number of bnodes in a gnode is equal to the maximum 
						  number of nodes*/
#define MAXBNODE_LINKS		0x100		/*The maximum number of gnodes a bnode is linked to*/
#define MAXBNODE_RNODEBLOCK	MAXBNODE_LINKS*MAXGROUPBNODE*sizeof(map_rnode)



/* * * Functions' declaration * * */
/*conversion functions*/
int pos_from_node(map_node *node, map_node *map);
map_node *node_from_pos(int pos, map_node *map);
void maptoip(u_int mapstart, u_int mapoff, inet_prefix ipstart, inet_prefix *ret);
int iptomap(u_int mapstart, inet_prefix ip, inet_prefix ipstart, u_int *ret);

map_node *init_map(size_t len);
void free_map(map_node *map, size_t count);
void map_node_del(map_node *node);

map_rnode *rnode_insert(map_rnode *buf, size_t pos, map_rnode *new);
map_rnode *map_rnode_insert(map_node *node, size_t pos, map_rnode *new);
map_rnode *rnode_add(map_node *node, map_rnode *new);
void rnode_swap(map_rnode *one, map_rnode *two);
void rnode_del(map_node *node, size_t pos);
void rnode_destroy(map_node *node);

map_bnode *map_bnode_del(map_bnode *bmap, u_int *bmap_nodes,  map_bnode *bnode);
int map_find_bnode(map_node *int_map, map_bnode *bmap, int count, map_node *node);

int rnode_rtt_compar(const void *a, const void *b);
void rnode_rtt_order(map_node *node);
int rnode_trtt_compar(const void *a, const void *b);
void rnode_trtt_order(map_node *node);
void map_routes_order(map_node *map);

int get_route_rtt(map_node *node, u_short route, struct timeval *rtt);
void rnode_set_trtt(map_node *node);
void rnode_recurse_trtt(map_rnode *rnode, int route, struct timeval *trtt);
void node_recurse_trtt(map_node *node);
void map_set_trtt(map_node *map);
map_node *get_gw_node(map_node *node, u_short route);

int merge_maps(map_node *base, map_node *new, map_node *base_root, map_node *new_root);
int mod_rnode_addr(map_rnode *node, int *map_start, int *new_start);
int get_rnode_block(int *map, map_node *node, map_rnode *rblock, int rstart);
map_rnode *map_get_rblock(map_node *map, int maxgroupnode, int *count);
int store_rnode_block(int *map, map_node *node, map_rnode *rblock, int rstart);
int map_store_rblock(map_node *map, int maxgroupnode, map_rnode *rblock, int count);
int save_map(map_node *map, map_node *root_node, char *file);
map_node *load_map(char *file);

int save_bmap(map_bnode *bmap, u_int bmap_nodes, char *file);
map_bnode *load_bmap(char *file, u_int *bmap_nodes);
