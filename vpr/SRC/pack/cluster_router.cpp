/* 
  Intra-logic block router determines if a candidate packing solution (or intermediate solution) can route.
  
  Global Inputs: Architecture and netlist
  Input arguments: clustering info for one cluster (t_pb info)
  Working data set: t_routing_data contains intermediate work
  Output: Routable? true/false.  If true, store/return the routed solution.

  Routing algorithm used is Pathfinder.

  Author: Jason Luu
  Date: July 22, 2013
 */

#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <queue>
#include <cmath>
using namespace std;

#include "vtr_assert.h"
#include "vtr_log.h"

#include "vpr_error.h"
#include "vpr_types.h"

#include "physical_types.h"
#include "globals.h"
#include "atom_netlist.h"
#include "vpr_utils.h"
#include "pack_types.h"
#include "lb_type_rr_graph.h"
#include "cluster_router.h"

/* #define PRINT_INTRA_LB_ROUTE */

/*****************************************************************************************
* Internal data structures
******************************************************************************************/

enum e_commit_remove {RT_COMMIT, RT_REMOVE};

// TODO: check if this hacky class memory reserve thing is still necessary, if not, then delete
/* Packing uses a priority queue that requires a large number of elements.  This backdoor
allows me to use a priority queue where I can pre-allocate the # of elements in the underlying container 
for efficiency reasons.  Note: Must use vector with this */
template <class T, class U, class V>
class reservable_pq: public priority_queue<T, U, V>
{
	public:
		typedef typename priority_queue<T>::size_type size_type;    
		reservable_pq(size_type capacity = 0) { 
			reserve(capacity); 
			cur_cap = capacity;
		};
		void reserve(size_type capacity) { 
			this->c.reserve(capacity); 
			cur_cap = capacity;
		}
		void clear() {
			this->c.clear();
			this->c.reserve(cur_cap); 			
		}
	private:
		size_type cur_cap;
};

/*****************************************************************************************
* Internal functions declarations
******************************************************************************************/
static void free_lb_net_rt(t_lb_trace *lb_trace);
static void free_lb_trace(t_lb_trace *lb_trace);
static void add_pin_to_rt_terminals(t_lb_router_data *router_data, const AtomPinId pin_id);
static void remove_pin_from_rt_terminals(t_lb_router_data *router_data, const AtomPinId pin_id);

static void fix_duplicate_equivalent_pins(t_lb_router_data *router_data);

static void commit_remove_rt(t_lb_trace *rt, t_lb_router_data *router_data, e_commit_remove op);
static bool is_skip_route_net(t_lb_trace *rt, t_lb_router_data *router_data);
static void add_source_to_rt(t_lb_router_data *router_data, int inet);
static void expand_rt(t_lb_router_data *router_data, int inet, reservable_pq<t_expansion_node, vector <t_expansion_node>, compare_expansion_node> &pq, int irt_net);
static void expand_rt_rec(t_lb_trace *rt, int prev_index, t_explored_node_tb *explored_node_tb, 
	reservable_pq<t_expansion_node, vector <t_expansion_node>, compare_expansion_node> &pq, int irt_net, int explore_id_index);
static void expand_node(t_lb_router_data *router_data, t_expansion_node exp_node, 
	reservable_pq<t_expansion_node, vector <t_expansion_node>, compare_expansion_node> &pq, int net_fanout);
static void add_to_rt(t_lb_trace *rt, int node_index, t_explored_node_tb *explored_node_tb, int irt_net);
static bool is_route_success(t_lb_router_data *router_data);
static t_lb_trace *find_node_in_rt(t_lb_trace *rt, int rt_index);
static void reset_explored_node_tb(t_lb_router_data *router_data);
static void save_and_reset_lb_route(t_lb_router_data *router_data);
static void load_trace_to_pb_route(t_pb_route *pb_route, const int total_pins, const AtomNetId net_id, const int prev_pin_id, const t_lb_trace *trace);

/*****************************************************************************************
* Debug functions declarations
******************************************************************************************/
#ifdef PRINT_INTRA_LB_ROUTE
static void print_route(char *filename, t_lb_router_data *router_data);
static void print_trace(FILE *fp, t_lb_trace *trace);
#endif

/*****************************************************************************************
* Constructor/Destructor functions 
******************************************************************************************/

/**
 Build data structures used by intra-logic block router
 */
t_lb_router_data *alloc_and_load_router_data(vector<t_lb_type_rr_node> *lb_type_graph, t_type_ptr type) {
	t_lb_router_data *router_data = new t_lb_router_data;
	int size;

	router_data->lb_type_graph = lb_type_graph;
	size = router_data->lb_type_graph->size();
	router_data->lb_rr_node_stats = new t_lb_rr_node_stats[size];
	router_data->explored_node_tb = new t_explored_node_tb[size];
	router_data->intra_lb_nets = new vector<t_intra_lb_net>;
	router_data->atoms_added = new map<AtomBlockId, bool>;
	router_data->lb_type = type;

	return router_data;
}

/* free data used by router */
void free_router_data(t_lb_router_data *router_data) {
	if(router_data != NULL && router_data->lb_type_graph != NULL) {
		delete [] router_data->lb_rr_node_stats;
		router_data->lb_rr_node_stats = NULL;
		delete [] router_data->explored_node_tb;
		router_data->explored_node_tb = NULL;
		router_data->lb_type_graph = NULL;
		delete router_data->atoms_added;
		router_data->atoms_added = NULL;
		free_intra_lb_nets(router_data->intra_lb_nets);
		free_intra_lb_nets(router_data->saved_lb_nets);
		router_data->intra_lb_nets = NULL;
		delete router_data;
	}
}


/*****************************************************************************************
* Routing Functions
******************************************************************************************/

/* Add pins of netlist atom to to current routing drivers/targets */
void add_atom_as_target(t_lb_router_data *router_data, const AtomBlockId blk_id) {
	const t_pb *pb;
    auto& atom_ctx = g_vpr_ctx.atom();

    std::map<AtomBlockId, bool>& atoms_added = *router_data->atoms_added;


	if(atoms_added.count(blk_id) > 0) {
		vpr_throw(VPR_ERROR_PACK, __FILE__, __LINE__, "Atom %s added twice to router\n", atom_ctx.nlist.block_name(blk_id).c_str());
	}

	pb = atom_ctx.lookup.atom_pb(blk_id);

    VTR_ASSERT(pb);
	
	atoms_added[blk_id] = true;

	set_reset_pb_modes(router_data, pb, true);

    for(auto pin_id : atom_ctx.nlist.block_pins(blk_id)) {
        add_pin_to_rt_terminals(router_data, pin_id);
    }

    fix_duplicate_equivalent_pins(router_data);
}

/* Remove pins of netlist atom from current routing drivers/targets */
void remove_atom_from_target(t_lb_router_data *router_data, const AtomBlockId blk_id) {
    auto& atom_ctx = g_vpr_ctx.atom();

	map <AtomBlockId, bool> & atoms_added = *router_data->atoms_added;

	const t_pb* pb = atom_ctx.lookup.atom_pb(blk_id);
	
	if(atoms_added.count(blk_id) == 0) {
		return;
	}
	
	set_reset_pb_modes(router_data, pb, false);
		
    for(auto pin_id : atom_ctx.nlist.block_pins(blk_id)) {
        remove_pin_from_rt_terminals(router_data, pin_id);
    }
    
	atoms_added.erase(blk_id);
}

/* Set/Reset mode of rr nodes to the pb used.  If set == true, then set all modes of the rr nodes affected by pb to the mode of the pb.
   Set all modes related to pb to 0 otherwise */
void set_reset_pb_modes(t_lb_router_data *router_data, const t_pb *pb, const bool set) {
	t_pb_type *pb_type;
	t_pb_graph_node *pb_graph_node;
	int mode = pb->mode;
	int inode;

    VTR_ASSERT(mode >= 0);
		
	pb_graph_node = pb->pb_graph_node;
	pb_type = pb_graph_node->pb_type;

	/* Input and clock pin modes are based on current pb mode */
	for(int iport = 0; iport < pb_graph_node->num_input_ports; iport++) {
		for(int ipin = 0; ipin < pb_graph_node->num_input_pins[iport]; ipin++) {
			inode = pb_graph_node->input_pins[iport][ipin].pin_count_in_cluster;
			router_data->lb_rr_node_stats[inode].mode = (set == true) ? mode : 0;
		}
	}
	for(int iport = 0; iport < pb_graph_node->num_clock_ports; iport++) {
		for(int ipin = 0; ipin < pb_graph_node->num_clock_pins[iport]; ipin++) {
			inode = pb_graph_node->clock_pins[iport][ipin].pin_count_in_cluster;
			router_data->lb_rr_node_stats[inode].mode = (set == true) ? mode : 0;
		}
	}

	/* Output pin modes are based on parent pb, so set children to use new mode 
	   Output pin of top-level logic block is also set to mode 0
	*/
	if(pb_type->num_modes != 0) {
		for(int ichild_type = 0; ichild_type < pb_type->modes[mode].num_pb_type_children; ichild_type++) {
			for(int ichild = 0; ichild < pb_type->modes[mode].pb_type_children[ichild_type].num_pb; ichild++) {
				t_pb_graph_node *child_pb_graph_node = &pb_graph_node->child_pb_graph_nodes[mode][ichild_type][ichild];
				for(int iport = 0; iport < child_pb_graph_node->num_output_ports; iport++) {
					for(int ipin = 0; ipin < child_pb_graph_node->num_output_pins[iport]; ipin++) {
						inode = child_pb_graph_node->output_pins[iport][ipin].pin_count_in_cluster;
						router_data->lb_rr_node_stats[inode].mode = (set == true) ? mode : 0;
					}
				}
			}
		}
	}
}

/* Attempt to route routing driver/targets on the current architecture 
   Follows pathfinder negotiated congestion algorithm
*/
bool try_intra_lb_route(t_lb_router_data *router_data) {
	vector <t_intra_lb_net> & lb_nets = *router_data->intra_lb_nets;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	bool is_routed = false;
	bool is_impossible = false;
	t_expansion_node exp_node;

	/* Stores state info during route */
	reservable_pq <t_expansion_node, vector <t_expansion_node>, compare_expansion_node> pq;

	reset_explored_node_tb(router_data);

	/* Reset current routing */
	for(unsigned int inet = 0; inet < lb_nets.size(); inet++) {
		free_lb_net_rt(lb_nets[inet].rt_tree);
		lb_nets[inet].rt_tree = NULL;
	}
	for(unsigned int inode = 0; inode < lb_type_graph.size(); inode++) {
		router_data->lb_rr_node_stats[inode].historical_usage = 0;
		router_data->lb_rr_node_stats[inode].occ = 0;
	}

	/*	Iteratively remove congestion until a successful route is found.  
		Cap the total number of iterations tried so that if a solution does not exist, then the router won't run indefinately */
	router_data->pres_con_fac = router_data->params.pres_fac;
	for(int iter = 0; iter < router_data->params.max_iterations && is_routed == false && is_impossible == false; iter++) {
		unsigned int inet;
		/* Iterate across all nets internal to logic block */
		for(inet = 0; inet < lb_nets.size() && is_impossible == false; inet++) {
			int idx = inet;
			if (is_skip_route_net(lb_nets[idx].rt_tree, router_data) == true) {
				continue;
			}
			commit_remove_rt(lb_nets[idx].rt_tree, router_data, RT_REMOVE);
			free_lb_net_rt(lb_nets[idx].rt_tree);
			lb_nets[idx].rt_tree = NULL;
			add_source_to_rt(router_data, idx);

			/* Route each sink of net */
			for(unsigned int itarget = 1; itarget < lb_nets[idx].terminals.size() && is_impossible == false; itarget++) {
				pq.clear();
				/* Get lowest cost next node, repeat until a path is found or if it is impossible to route */
				expand_rt(router_data, idx, pq, idx);
				do {
					if(pq.empty()) {
						/* No connection possible */
						is_impossible = true;
					} else {
						exp_node = pq.top();
						pq.pop();

                        if(router_data->explored_node_tb[exp_node.node_index].explored_id != router_data->explore_id_index) {
							/* First time node is popped implies path to this node is the lowest cost.
								If the node is popped a second time, then the path to that node is higher than this path so
								ignore. 
							*/
							router_data->explored_node_tb[exp_node.node_index].explored_id = router_data->explore_id_index;
							router_data->explored_node_tb[exp_node.node_index].prev_index = exp_node.prev_index;
							if(exp_node.node_index != lb_nets[idx].terminals[itarget]) {								
								expand_node(router_data, exp_node, pq, lb_nets[idx].terminals.size() - 1);
							}
                        }
					}
				} while(exp_node.node_index != lb_nets[idx].terminals[itarget] && is_impossible == false);

				if(exp_node.node_index == lb_nets[idx].terminals[itarget]) {
					/* Net terminal is routed, add this to the route tree, clear data structures, and keep going */
					add_to_rt(lb_nets[idx].rt_tree, exp_node.node_index, router_data->explored_node_tb, idx);					
				}

				router_data->explore_id_index++;
				if(router_data->explore_id_index > 2000000000) {
					/* overflow protection */
					for(unsigned int id = 0; id < lb_type_graph.size(); id++) {
						router_data->explored_node_tb[id].explored_id = OPEN;
						router_data->explored_node_tb[id].enqueue_id = OPEN;
						router_data->explore_id_index = 1;
					}
				}								
			}
			
			commit_remove_rt(lb_nets[idx].rt_tree, router_data, RT_COMMIT);
		}
		if(is_impossible == false) {
			is_routed = is_route_success(router_data);
		} else {
			--inet;
            auto& atom_ctx = g_vpr_ctx.atom();
			vtr::printf_info("Routing net %s is impossible\n", atom_ctx.nlist.net_name(lb_nets[inet].atom_net_id).c_str());
			is_routed = false;
		}
		router_data->pres_con_fac *= router_data->params.pres_fac_mult;
	}

	if (is_routed) {
		save_and_reset_lb_route(router_data);
	} else {
		for (unsigned int inet = 0; inet < lb_nets.size(); inet++) {
			free_lb_net_rt(lb_nets[inet].rt_tree);
			lb_nets[inet].rt_tree = NULL;
		}
#ifdef PRINT_INTRA_LB_ROUTE
		print_route("intra_lb_failed_route.echo", router_data);
#endif
	}
	return is_routed;
}


/*****************************************************************************************
* Accessor Functions
******************************************************************************************/

/* Creates an array [0..num_pb_graph_pins-1] lookup for intra-logic block routing.  Given pb_graph_pin id for clb, lookup atom net that uses that pin.
   If pin is not used, stores OPEN at that pin location */
t_pb_route *alloc_and_load_pb_route(const vector <t_intra_lb_net> *intra_lb_nets, t_pb_graph_node *pb_graph_head) {
	const vector <t_intra_lb_net> &lb_nets = *intra_lb_nets;
	int total_pins = pb_graph_head->total_pb_pins;
	t_pb_route * pb_route = new t_pb_route[pb_graph_head->total_pb_pins];

	for(int ipin = 0; ipin < total_pins; ipin++) {
		pb_route[ipin].atom_net_id = AtomNetId::INVALID();
		pb_route[ipin].driver_pb_pin_id = OPEN;
	}

	for(int inet = 0; inet < (int)lb_nets.size(); inet++) {
		load_trace_to_pb_route(pb_route, total_pins, lb_nets[inet].atom_net_id, OPEN, lb_nets[inet].rt_tree);
	}

	return pb_route;
}

/* Free pin-to-atomic_net array lookup */
void free_pb_route(t_pb_route *pb_route) {
	if(pb_route != NULL) {
		delete []pb_route;
	}
}

void free_intra_lb_nets(vector <t_intra_lb_net> *intra_lb_nets) {
	if(intra_lb_nets == NULL) {
		return;
	}
	vector <t_intra_lb_net> &lb_nets = *intra_lb_nets;
	for(unsigned int i = 0; i < lb_nets.size(); i++) {
		lb_nets[i].terminals.clear();
		free_lb_net_rt(lb_nets[i].rt_tree);
		lb_nets[i].rt_tree = NULL;
	}		
	delete intra_lb_nets;
}


/***************************************************************************
Internal Functions
****************************************************************************/

/* Recurse through route tree trace to populate pb pin to atom net lookup array */
static void load_trace_to_pb_route(t_pb_route *pb_route, const int total_pins, const AtomNetId net_id, const int prev_pin_id, const t_lb_trace *trace) {
	int ipin = trace->current_node;
	int driver_pb_pin_id = prev_pin_id;
	int cur_pin_id = OPEN;
	if(ipin < total_pins) {
		/* This routing node corresponds with a pin.  This node is virtual (ie. sink or source node) */
		cur_pin_id = ipin;
		if(!pb_route[cur_pin_id].atom_net_id) {
			pb_route[cur_pin_id].atom_net_id = net_id;
			pb_route[cur_pin_id].driver_pb_pin_id = driver_pb_pin_id;
		} else {
			VTR_ASSERT(pb_route[cur_pin_id].atom_net_id == net_id);
		}		
	}
	for(int itrace = 0; itrace< (int)trace->next_nodes.size(); itrace++) {
		load_trace_to_pb_route(pb_route, total_pins, net_id, cur_pin_id, &trace->next_nodes[itrace]);
	}
}


/* Free route tree for intra-logic block routing */
static void free_lb_net_rt(t_lb_trace *lb_trace) {
	if(lb_trace != NULL) {
		for(unsigned int i = 0; i < lb_trace->next_nodes.size(); i++) {
			free_lb_trace(&lb_trace->next_nodes[i]);
		}
		lb_trace->next_nodes.clear();
		delete lb_trace;
	}
}

/* Free trace for intra-logic block routing */
static void free_lb_trace(t_lb_trace *lb_trace) {
	if(lb_trace != NULL) {
		for(unsigned int i = 0; i < lb_trace->next_nodes.size(); i++) {
			free_lb_trace(&lb_trace->next_nodes[i]);
		}
		lb_trace->next_nodes.clear();
	}
}


/* Given a pin of a net, assign route tree terminals for it 
   Assumes that pin is not already assigned
*/
static void add_pin_to_rt_terminals(t_lb_router_data *router_data, const AtomPinId pin_id) {
	vector <t_intra_lb_net> & lb_nets = *router_data->intra_lb_nets;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	t_type_ptr lb_type = router_data->lb_type;
	bool found = false;
	unsigned int ipos;
    auto& atom_ctx = g_vpr_ctx.atom();

    const t_pb_graph_pin* pb_graph_pin = find_pb_graph_pin(atom_ctx.nlist, atom_ctx.lookup, pin_id);
    VTR_ASSERT(pb_graph_pin);

    AtomPortId port_id = atom_ctx.nlist.pin_port(pin_id);
    AtomNetId net_id = atom_ctx.nlist.pin_net(pin_id);

	if(!net_id) {
		/* This is not a valid net */
		return;
	}

	/* Find if current net is in route tree, if not, then add to rt.
	   Code assumes that # of nets in cluster is small so a linear search through 
       vector is faster than using more complex data structures
	*/
	for(ipos = 0; ipos < lb_nets.size(); ipos++) {
		if(lb_nets[ipos].atom_net_id == net_id) {
			found = true;
			break;
		}
	}
	if(found == false) {
		struct t_intra_lb_net new_net;

        new_net.atom_net_id = net_id;

		ipos = lb_nets.size();
		lb_nets.push_back(new_net);
	}
	VTR_ASSERT(lb_nets[ipos].atom_net_id == net_id);
    VTR_ASSERT(lb_nets[ipos].atom_pins.size() == lb_nets[ipos].terminals.size());

	/*
	Determine whether or not this is a new intra lb net, if yes, then add to list of intra lb nets
	*/	
	if(lb_nets[ipos].terminals.empty()) {
		/* Add terminals */
		int source_terminal;
		source_terminal = get_lb_type_rr_graph_ext_source_index(lb_type);
		lb_nets[ipos].terminals.push_back(source_terminal);
		lb_nets[ipos].atom_pins.push_back(pin_id);
		VTR_ASSERT(lb_type_graph[lb_nets[ipos].terminals[0]].type == LB_SOURCE);

	}

    VTR_ASSERT(lb_nets[ipos].atom_pins.size() == lb_nets[ipos].terminals.size());

	if(atom_ctx.nlist.port_type(port_id) == AtomPortType::OUTPUT) {
		/* Net driver pin takes 0th position in terminals */
		int sink_terminal;
		VTR_ASSERT(lb_nets[ipos].terminals[0] == get_lb_type_rr_graph_ext_source_index(lb_type));
		lb_nets[ipos].terminals[0] = pb_graph_pin->pin_count_in_cluster;
		lb_nets[ipos].atom_pins[0] = pin_id;

		VTR_ASSERT(lb_type_graph[lb_nets[ipos].terminals[0]].type == LB_SOURCE);
		

		if(lb_nets[ipos].terminals.size() < atom_ctx.nlist.net_pins(net_id).size()) {
			/* Must route out of cluster, put out of cluster sink terminal as first terminal */
			if(lb_nets[ipos].terminals.size() == 1) {
				sink_terminal = get_lb_type_rr_graph_ext_sink_index(lb_type);
				lb_nets[ipos].terminals.push_back(sink_terminal);
				lb_nets[ipos].atom_pins.push_back(pin_id);
			} else {
				sink_terminal = lb_nets[ipos].terminals[1];
				lb_nets[ipos].terminals.push_back(sink_terminal);
				lb_nets[ipos].atom_pins.push_back(pin_id);

				sink_terminal = get_lb_type_rr_graph_ext_sink_index(lb_type);
				lb_nets[ipos].terminals[1] = sink_terminal;
				lb_nets[ipos].atom_pins[1] = pin_id;
			}
			VTR_ASSERT(lb_type_graph[lb_nets[ipos].terminals[1]].type == LB_SINK);
		}
	} else {
        //This is an input to a primitive
        VTR_ASSERT(atom_ctx.nlist.port_type(port_id) == AtomPortType::INPUT
                   || atom_ctx.nlist.port_type(port_id) == AtomPortType::CLOCK);

        //Get the rr node index associated with the pin
		int pin_index = pb_graph_pin->pin_count_in_cluster;
		VTR_ASSERT(get_num_modes_of_lb_type_rr_node(lb_type_graph[pin_index]) == 1);
		VTR_ASSERT(lb_type_graph[pin_index].num_fanout[0] == 1);
		
		/* We actually route to the sink (to handle logically equivalent pins).
         * The sink is one past the primitive input pin */
		int sink_index = lb_type_graph[pin_index].outedges[0][0].node_index;
		VTR_ASSERT(lb_type_graph[sink_index].type == LB_SINK);

		if(lb_nets[ipos].terminals.size() == atom_ctx.nlist.net_pins(net_id).size() &&
			lb_nets[ipos].terminals[1] == get_lb_type_rr_graph_ext_sink_index(lb_type)) {

		    /* If all sinks of net are all contained in the logic block, then the net does 
             * not need to route out of the logic block, so the external sink can be removed */
			lb_nets[ipos].terminals[1] = sink_index;
			lb_nets[ipos].atom_pins[1] = pin_id;
		} else {
            //Add as a regular sink
			lb_nets[ipos].terminals.push_back(sink_index);
            lb_nets[ipos].atom_pins.push_back(pin_id);
		}
	}
    VTR_ASSERT(lb_nets[ipos].atom_pins.size() == lb_nets[ipos].terminals.size());

	int num_lb_terminals = lb_nets[ipos].terminals.size();
	VTR_ASSERT(num_lb_terminals <= (int) atom_ctx.nlist.net_pins(net_id).size());
	VTR_ASSERT(num_lb_terminals >= 0);
}



/* Given a pin of a net, remove route tree terminals from it 
*/
static void remove_pin_from_rt_terminals(t_lb_router_data *router_data, const AtomPinId pin_id) {
	vector <t_intra_lb_net> & lb_nets = *router_data->intra_lb_nets;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	t_type_ptr lb_type = router_data->lb_type;
	bool found = false;
	unsigned int ipos;
    auto& atom_ctx = g_vpr_ctx.atom();

    const t_pb_graph_pin* pb_graph_pin = find_pb_graph_pin(atom_ctx.nlist, atom_ctx.lookup, pin_id);

    AtomPortId port_id = atom_ctx.nlist.pin_port(pin_id);
    AtomNetId net_id = atom_ctx.nlist.pin_net(pin_id);

	if(!net_id) {
		/* This is not a valid net */
		return;
	}
	
	/* Find current net in route tree
	   Code assumes that # of nets in cluster is small so a linear search through vector is faster than using more complex data structures
	*/
	for(ipos = 0; ipos < lb_nets.size(); ipos++) {
		if(lb_nets[ipos].atom_net_id == net_id) {
			found = true;
			break;
		}
	}
	VTR_ASSERT(found == true);
	VTR_ASSERT(lb_nets[ipos].atom_net_id == net_id);
	
    VTR_ASSERT(lb_nets[ipos].atom_pins.size() == lb_nets[ipos].terminals.size());

    auto port_type = atom_ctx.nlist.port_type(port_id);
	if(port_type == AtomPortType::OUTPUT) {
		/* Net driver pin takes 0th position in terminals */
		int sink_terminal;
		VTR_ASSERT(lb_nets[ipos].terminals[0] == pb_graph_pin->pin_count_in_cluster);
		lb_nets[ipos].terminals[0] = get_lb_type_rr_graph_ext_source_index(lb_type);		

		/* source terminal is now coming from outside logic block, do not need to route signal out of logic block */
		sink_terminal = get_lb_type_rr_graph_ext_sink_index(lb_type);
		if(lb_nets[ipos].terminals[1] == sink_terminal) {
			lb_nets[ipos].terminals[1] = lb_nets[ipos].terminals.back();
			lb_nets[ipos].terminals.pop_back();

            lb_nets[ipos].atom_pins[1] = lb_nets[ipos].atom_pins.back();
            lb_nets[ipos].atom_pins.pop_back();
		}
	} else {
        VTR_ASSERT(port_type == AtomPortType::INPUT || port_type == AtomPortType::CLOCK);

		/* Remove sink from list of terminals */
		int pin_index = pb_graph_pin->pin_count_in_cluster;
		unsigned int iterm;


		VTR_ASSERT(get_num_modes_of_lb_type_rr_node(lb_type_graph[pin_index]) == 1);
		VTR_ASSERT(lb_type_graph[pin_index].num_fanout[0] == 1);
		int sink_index = lb_type_graph[pin_index].outedges[0][0].node_index;
		VTR_ASSERT(lb_type_graph[sink_index].type == LB_SINK);
			
        int target_index = -1;
        //Search for the sink
		found = false;
		for(iterm = 0; iterm < lb_nets[ipos].terminals.size(); iterm++) {
			if(lb_nets[ipos].terminals[iterm] == sink_index) {
                target_index = sink_index;
				found = true;
				break;
			}
		}
        if(!found) {
            //Search for the pin
            found = false;
            for(iterm = 0; iterm < lb_nets[ipos].terminals.size(); iterm++) {
                if(lb_nets[ipos].terminals[iterm] == pin_index) {
                    target_index = pin_index;
                    found = true;
                    break;
                }
            }

        }
		VTR_ASSERT(found == true);
		VTR_ASSERT(lb_nets[ipos].terminals[iterm] == target_index);
		VTR_ASSERT(iterm > 0);
		
		/* Drop terminal from list */
		lb_nets[ipos].terminals[iterm] = lb_nets[ipos].terminals.back();
		lb_nets[ipos].terminals.pop_back();

        lb_nets[ipos].atom_pins[iterm] = lb_nets[ipos].atom_pins.back();
        lb_nets[ipos].atom_pins.pop_back();
		
		if(lb_nets[ipos].terminals.size() == 1 && lb_nets[ipos].terminals[0] != get_lb_type_rr_graph_ext_source_index(lb_type)) {
			/* The removed sink must be driven by an atom found in the cluster, add in special sink outside of cluster to represent this */
			lb_nets[ipos].terminals.push_back(get_lb_type_rr_graph_ext_sink_index(lb_type));
            lb_nets[ipos].atom_pins.push_back(AtomPinId::INVALID());
		}

		if(lb_nets[ipos].terminals.size() > 1 &&
			lb_nets[ipos].terminals[1] != get_lb_type_rr_graph_ext_sink_index(lb_type) && 
			lb_nets[ipos].terminals[0] != get_lb_type_rr_graph_ext_source_index(lb_type)) {
			
			/* The removed sink must be driven by an atom found in the cluster, add in special sink outside of cluster to represent this */
			int terminal = lb_nets[ipos].terminals[1];
			lb_nets[ipos].terminals.push_back(terminal);
			lb_nets[ipos].terminals[1] = get_lb_type_rr_graph_ext_sink_index(lb_type);

			AtomPinId pin = lb_nets[ipos].atom_pins[1];
			lb_nets[ipos].atom_pins.push_back(pin);
			lb_nets[ipos].atom_pins[1] = AtomPinId::INVALID();

		}
	}
    VTR_ASSERT(lb_nets[ipos].atom_pins.size() == lb_nets[ipos].terminals.size());

	if(lb_nets[ipos].terminals.size() == 1 && 
		lb_nets[ipos].terminals[0] == get_lb_type_rr_graph_ext_source_index(lb_type)) {
		/* This net is not routed, remove from list of nets in lb */
		lb_nets[ipos] = lb_nets.back();
		lb_nets.pop_back();
	}
}

//It is possible that a net may connect multiple times to a logically equivalent set of primitive pins.
//The cluster router will only route one connection for a particular net to the common sink of the
//equivalent pins.
//
//To work around this, we fix all but one of these duplicate connections to route to specific pins,
//(instead of the common sink). This ensures a legal routing is produced and that the duplicate pins
//are not 'missing' in the clustered netlist.
static void fix_duplicate_equivalent_pins(t_lb_router_data *router_data) {
    auto& atom_ctx = g_vpr_ctx.atom();

	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	vector <t_intra_lb_net> & lb_nets = *router_data->intra_lb_nets;

    for(size_t ilb_net = 0; ilb_net < lb_nets.size(); ++ilb_net) {

        //Collect all the sink terminals indicies which target a particular node
        std::map<int,std::vector<int>> duplicate_terminals;
        for(size_t iterm = 1; iterm < lb_nets[ilb_net].terminals.size(); ++iterm) {
            int node = lb_nets[ilb_net].terminals[iterm];

            duplicate_terminals[node].push_back(iterm);
        }

        for(auto kv : duplicate_terminals) {
            if(kv.second.size() < 2) continue; //Only process duplicates

            //Remap all the duplicate terminals so they target the pin instead of the sink
            for(size_t idup_term = 0; idup_term < kv.second.size(); ++idup_term) {
                int iterm = kv.second[idup_term]; //The index in terminals which is duplicated

                VTR_ASSERT(lb_nets[ilb_net].atom_pins.size() == lb_nets[ilb_net].terminals.size());
                AtomPinId atom_pin = lb_nets[ilb_net].atom_pins[iterm];
                VTR_ASSERT(atom_pin);

                const t_pb_graph_pin* pb_graph_pin = find_pb_graph_pin(atom_ctx.nlist, atom_ctx.lookup, atom_pin);
                VTR_ASSERT(pb_graph_pin);

                if(!pb_graph_pin->port->equivalent) continue; //Only need to remap equivalent ports

                //Remap this terminal to an explicit pin instead of the common sink
                int pin_index = pb_graph_pin->pin_count_in_cluster;

                vtr::printf_warning(__FILE__, __LINE__, 
                            "Found duplicate nets connected to logically equivalent pins. "
                            "Remapping intra lb net %d (atom net %zu '%s') from common sink "
                            "pb_route %d to fixed pin pb_route %d\n",
                            ilb_net, size_t(lb_nets[ilb_net].atom_net_id), atom_ctx.nlist.net_name(lb_nets[ilb_net].atom_net_id).c_str(),
                            kv.first, pin_index);

                VTR_ASSERT(lb_type_graph[pin_index].type == LB_INTERMEDIATE);
                VTR_ASSERT(lb_type_graph[pin_index].num_fanout[0] == 1);
                int sink_index = lb_type_graph[pin_index].outedges[0][0].node_index;
                VTR_ASSERT(lb_type_graph[sink_index].type == LB_SINK);
                VTR_ASSERT_MSG(sink_index == lb_nets[ilb_net].terminals[iterm], "Remapped pin must be connected to original sink");


                //Change the target
                lb_nets[ilb_net].terminals[iterm] = pin_index;
            }
        }
    }
}

/* Commit or remove route tree from currently routed solution */
static void commit_remove_rt(t_lb_trace *rt, t_lb_router_data *router_data, e_commit_remove op) {
	t_lb_rr_node_stats *lb_rr_node_stats;
	t_explored_node_tb *explored_node_tb;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	int inode;
	int incr;

	lb_rr_node_stats = router_data->lb_rr_node_stats;
	explored_node_tb = router_data->explored_node_tb;

	if(rt == NULL) {
		return;
	}

	inode = rt->current_node;

	/* Determine if node is being used or removed */
	if (op == RT_COMMIT) {
		incr = 1;
		if (lb_rr_node_stats[inode].occ >= lb_type_graph[inode].capacity) {
			lb_rr_node_stats[inode].historical_usage += (lb_rr_node_stats[inode].occ - lb_type_graph[inode].capacity + 1); /* store historical overuse */
		}		
	} else {
		incr = -1;
		explored_node_tb[inode].inet = OPEN;
	}

	lb_rr_node_stats[inode].occ += incr;
	VTR_ASSERT(lb_rr_node_stats[inode].occ >= 0);

	/* Recursively update route tree */
	for(unsigned int i = 0; i < rt->next_nodes.size(); i++) {
		commit_remove_rt(&rt->next_nodes[i], router_data, op);
	}
}

/* Should net be skipped?  If the net does not conflict with another net, then skip routing this net */
static bool is_skip_route_net(t_lb_trace *rt, t_lb_router_data *router_data) {
	t_lb_rr_node_stats *lb_rr_node_stats;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	int inode;
	
	lb_rr_node_stats = router_data->lb_rr_node_stats;
	
	if (rt == NULL) {
		return false; /* Net is not routed, therefore must route net */
	}

	inode = rt->current_node;

	/* Determine if node is overused */
	if (lb_rr_node_stats[inode].occ > lb_type_graph[inode].capacity) {
		/* Conflict between this net and another net at this node, reroute net */
		return false;
	}

	/* Recursively check that rest of route tree does not have a conflict */
	for (unsigned int i = 0; i < rt->next_nodes.size(); i++) {
		if (is_skip_route_net(&rt->next_nodes[i], router_data) == false) {
			return false;
		}
	}

	/* No conflict, this net's current route is legal, skip routing this net */
	return true;
}


/* At source mode as starting point to existing route tree */
static void add_source_to_rt(t_lb_router_data *router_data, int inet) {
	VTR_ASSERT((*router_data->intra_lb_nets)[inet].rt_tree == NULL);
	(*router_data->intra_lb_nets)[inet].rt_tree = new t_lb_trace;
	(*router_data->intra_lb_nets)[inet].rt_tree->current_node = (*router_data->intra_lb_nets)[inet].terminals[0];
}

/* Expand all nodes found in route tree into priority queue */
static void expand_rt(t_lb_router_data *router_data, int inet, 
	reservable_pq<t_expansion_node, vector <t_expansion_node>, compare_expansion_node> &pq, int irt_net) {

	vector<t_intra_lb_net> &lb_nets = *router_data->intra_lb_nets;
	
	VTR_ASSERT(pq.empty());

	expand_rt_rec(lb_nets[inet].rt_tree, OPEN, router_data->explored_node_tb, pq, irt_net, router_data->explore_id_index);
}


/* Expand all nodes found in route tree into priority queue recursively */
static void expand_rt_rec(t_lb_trace *rt, int prev_index, t_explored_node_tb *explored_node_tb, 
	reservable_pq<t_expansion_node, vector <t_expansion_node>, compare_expansion_node> &pq, int irt_net, int explore_id_index) {
	
	t_expansion_node enode;

	/* Perhaps should use a cost other than zero */
	enode.cost = 0;
	enode.node_index = rt->current_node;
	enode.prev_index = prev_index;
	pq.push(enode);
	explored_node_tb[enode.node_index].inet = irt_net;
	explored_node_tb[enode.node_index].explored_id = OPEN;
	explored_node_tb[enode.node_index].enqueue_id = explore_id_index;
	explored_node_tb[enode.node_index].enqueue_cost = 0;
	explored_node_tb[enode.node_index].prev_index = prev_index;
	

	for(unsigned int i = 0; i < rt->next_nodes.size(); i++) {
		expand_rt_rec(&rt->next_nodes[i], rt->current_node, explored_node_tb, pq, irt_net, explore_id_index);
	}
}


/* Expand all nodes found in route tree into priority queue */
static void expand_node(t_lb_router_data *router_data, t_expansion_node exp_node, 
	reservable_pq<t_expansion_node, vector <t_expansion_node>, compare_expansion_node> &pq, int net_fanout) {

	int cur_node;
	float cur_cost, incr_cost;
	int mode, usage;
	t_expansion_node enode;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	t_lb_rr_node_stats *lb_rr_node_stats = router_data->lb_rr_node_stats;

	cur_node = exp_node.node_index;
	cur_cost = exp_node.cost;
	mode = lb_rr_node_stats[cur_node].mode;
	t_lb_router_params params = router_data->params;


	for(int iedge = 0; iedge < lb_type_graph[cur_node].num_fanout[mode]; iedge++) {
		int next_mode;

		/* Init new expansion node */
		enode.prev_index = cur_node;
		enode.node_index = lb_type_graph[cur_node].outedges[mode][iedge].node_index;
		enode.cost = cur_cost;

		/* Determine incremental cost of using expansion node */
		usage = lb_rr_node_stats[enode.node_index].occ + 1 - lb_type_graph[enode.node_index].capacity;
		incr_cost = lb_type_graph[enode.node_index].intrinsic_cost;
		incr_cost += lb_type_graph[cur_node].outedges[mode][iedge].intrinsic_cost;
		incr_cost += params.hist_fac * lb_rr_node_stats[enode.node_index].historical_usage;
		if(usage > 0) {
			incr_cost *= (usage * router_data->pres_con_fac);
		}		
				
		/* Adjust cost so that higher fanout nets prefer higher fanout routing nodes while lower fanout nets prefer lower fanout routing nodes */
		float fanout_factor = 1.0;
		next_mode = lb_rr_node_stats[enode.node_index].mode;
        VTR_ASSERT(next_mode >= 0);
		if (lb_type_graph[enode.node_index].num_fanout[next_mode] > 1) {
			fanout_factor = 0.85 + (0.25 / net_fanout);
		}
		else {
			fanout_factor = 1.15 - (0.25 / net_fanout);
		}
		incr_cost *= fanout_factor;
		enode.cost = cur_cost + incr_cost;


		/* Add to queue if cost is lower than lowest cost path to this enode */
		if(router_data->explored_node_tb[enode.node_index].enqueue_id == router_data->explore_id_index) {
			if(enode.cost < router_data->explored_node_tb[enode.node_index].enqueue_cost) {
				pq.push(enode);
			}
		} else {
			router_data->explored_node_tb[enode.node_index].enqueue_id = router_data->explore_id_index;
			router_data->explored_node_tb[enode.node_index].enqueue_cost = enode.cost;
			pq.push(enode);
		}
	}
	
}



/* Add new path from existing route tree to target sink */
static void add_to_rt(t_lb_trace *rt, int node_index, t_explored_node_tb *explored_node_tb, int irt_net) {
	vector <int> trace_forward;	
	int rt_index, trace_index;
	t_lb_trace *link_node;
	t_lb_trace curr_node;

	/* Store path all the way back to route tree */
	rt_index = node_index;
	while(explored_node_tb[rt_index].inet != irt_net) {
        trace_forward.push_back(rt_index);
		rt_index = explored_node_tb[rt_index].prev_index;
		VTR_ASSERT(rt_index != OPEN);
	}

	/* Find rt_index on the route tree */
	link_node = find_node_in_rt(rt, rt_index);
	VTR_ASSERT(link_node != NULL);

	/* Add path to root tree */
	while(!trace_forward.empty()) {
		trace_index = trace_forward.back();
        curr_node.current_node = trace_index;
        link_node->next_nodes.push_back(curr_node);
        link_node = &link_node->next_nodes.back();
        trace_forward.pop_back();
	}
}

/* Determine if a completed route is valid.  A successful route has no congestion (ie. no routing resource is used by two nets). */
static bool is_route_success(t_lb_router_data *router_data) {
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;

	for(unsigned int inode = 0; inode < lb_type_graph.size(); inode++) {
		if(router_data->lb_rr_node_stats[inode].occ > lb_type_graph[inode].capacity) {
			return false;
		}
	}

	return true;
}

/* Given a route tree and an index of a node on the route tree, return a pointer to the trace corresponding to that index */
static t_lb_trace *find_node_in_rt(t_lb_trace *rt, int rt_index) {
	t_lb_trace *cur;
	if(rt->current_node == rt_index) {
		return rt;
	} else {
		for(unsigned int i = 0; i < rt->next_nodes.size(); i++) {
			cur = find_node_in_rt(&rt->next_nodes[i], rt_index);
			if(cur != NULL) {
				return cur;
			}
		}
	}
	return NULL;
}

#ifdef PRINT_INTRA_LB_ROUTE
/* Debug routine, print out current intra logic block route */
static void print_route(char *filename, t_lb_router_data *router_data) {
	FILE *fp;
	vector <t_intra_lb_net> & lb_nets = *router_data->intra_lb_nets;
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;

	fp = my_fopen(filename, "w", 0);
	
	for(unsigned int inode = 0; inode < lb_type_graph.size(); inode++) {
		fprintf(fp, "node %d occ %d cap %d\n", inode, router_data->lb_rr_node_stats[inode].occ, lb_type_graph[inode].capacity);
	}

	fprintf(fp, "\n\n----------------------------------------------------\n\n");

    auto& atom_ctx = g_vpr_ctx.atom();

	for(unsigned int inet = 0; inet < lb_nets.size(); inet++) {
		AtomNetId net_id = lb_nets[inet].atom_net_id;
		fprintf(fp, "net %s num targets %d \n", atom_ctx.nlist.net_name(net_id).c_str(), (int)lb_nets[inet].terminals.size());
		print_trace(fp, lb_nets[inet].rt_tree);
		fprintf(fp, "\n\n");
	}
	fclose(fp);
}

/* Debug routine, print out trace of net */
static void print_trace(FILE *fp, t_lb_trace *trace) {
	if(trace == NULL) {
		fprintf(fp, "NULL");
		return;
	}
	for(unsigned int ibranch = 0; ibranch < trace->next_nodes.size(); ibranch++) {
		if(trace->next_nodes.size() > 1) {
			fprintf(fp, "B(%d-->%d) ", trace->current_node, trace->next_nodes[ibranch].current_node);
		} else {
			fprintf(fp, "(%d-->%d) ", trace->current_node, trace->next_nodes[ibranch].current_node);
		}
		print_trace(fp, &trace->next_nodes[ibranch]);
	}
}
#endif

static void reset_explored_node_tb(t_lb_router_data *router_data) {
	vector <t_lb_type_rr_node> & lb_type_graph = *router_data->lb_type_graph;
	for(unsigned int inode = 0; inode < lb_type_graph.size(); inode++) {
		router_data->explored_node_tb[inode].prev_index = OPEN;
		router_data->explored_node_tb[inode].explored_id = OPEN;
		router_data->explored_node_tb[inode].inet = OPEN;
		router_data->explored_node_tb[inode].enqueue_id = OPEN;
		router_data->explored_node_tb[inode].enqueue_cost = 0;
	}
}



/* Save last successful intra-logic block route and reset current traceback */
static void save_and_reset_lb_route(t_lb_router_data *router_data) {
	vector <t_intra_lb_net> & lb_nets = *router_data->intra_lb_nets;

	/* Free old saved lb nets if exist */
	if(router_data->saved_lb_nets != NULL) {		
		free_intra_lb_nets(router_data->saved_lb_nets);
		router_data->saved_lb_nets = NULL;
	}

	/* Save current routed solution */
	router_data->saved_lb_nets = new vector<t_intra_lb_net> (lb_nets.size());
	vector <t_intra_lb_net> & saved_lb_nets = *router_data->saved_lb_nets;

	for(int inet = 0; inet < (int)saved_lb_nets.size(); inet++) {
		/* 
		  Save and reset route tree data
		*/
		saved_lb_nets[inet].atom_net_id = lb_nets[inet].atom_net_id;
		saved_lb_nets[inet].terminals.resize(lb_nets[inet].terminals.size());
		for(int iterm = 0; iterm < (int) lb_nets[inet].terminals.size(); iterm++) {
			saved_lb_nets[inet].terminals[iterm] = lb_nets[inet].terminals[iterm];
		}
		saved_lb_nets[inet].rt_tree = lb_nets[inet].rt_tree;
		lb_nets[inet].rt_tree = NULL;
	}
}



