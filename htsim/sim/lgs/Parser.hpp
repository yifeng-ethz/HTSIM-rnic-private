#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <string>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "LogGOPSim.hpp"

#define MAGIC_COOKIE 4223
#define MAGIC_COOKIE_INVALID 2342


#define OPTYPE_SEND 1
#define OPTYPE_RECV 2
#define OPTYPE_CALC 3

typedef uint64_t base_t;

namespace goal_binary_detail {

template <typename T>
T load_packed(const char* source) {
	static_assert(std::is_trivially_copyable<T>::value,
	              "packed GOAL scalars must be trivially copyable");
	T value;
	std::memcpy(&value, source, sizeof(value));
	return value;
}

template <typename T>
void store_packed(char* destination, const T& value) {
	static_assert(std::is_trivially_copyable<T>::value,
	              "packed GOAL scalars must be trivially copyable");
	std::memcpy(destination, &value, sizeof(value));
}

inline std::runtime_error malformed(const std::string& detail) {
	return std::runtime_error("malformed serialized GOAL schedule: " + detail);
}

inline size_t checked_add(size_t lhs, size_t rhs, const char* field) {
	if (rhs > std::numeric_limits<size_t>::max() - lhs) {
		throw malformed(std::string(field) + " size overflows");
	}
	return lhs + rhs;
}

inline size_t checked_multiply(size_t lhs, size_t rhs, const char* field) {
	if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
		throw malformed(std::string(field) + " size overflows");
	}
	return lhs * rhs;
}

template <typename T>
T load_bounded(const char* base,
	           size_t length,
	           size_t offset,
	           const char* field) {
	if (offset > length || sizeof(T) > length - offset) {
		throw malformed(std::string(field) + " is outside its byte span");
	}
	return load_packed<T>(base + offset);
}

template <typename T>
void store_bounded(char* base,
	              size_t length,
	              size_t offset,
	              const T& value,
	              const char* field) {
	if (offset > length || sizeof(T) > length - offset) {
		throw malformed(std::string(field) + " is outside its byte span");
	}
	store_packed(base + offset, value);
}

}  // namespace goal_binary_detail

// TODO Experiment with packing of the node structure,
//      that could save us up to 30% in space, but might
//      harm time.

struct Node {
	
	uint64_t           Size;
	std::vector<Node*> DependOnMe;
	std::vector<Node*> StartDependOnMe;
	uint32_t           DependenciesCnt;
	uint32_t           offset;
	uint32_t           Peer;
	uint32_t           Tag;
	uint8_t            Proc;
	uint8_t            Nic;
	char               Type;
	
};

struct DeserializedNode {

	uint32_t           DependenciesCnt;
	char               Type;
	uint32_t           Peer;
	uint64_t           Size;
	uint32_t           Tag;
	uint8_t            Proc;
	uint8_t            Nic;
	uint32_t           offset;
	uint64_t		   start_time;
	std::vector<uint32_t> DependOnMe;
	std::vector<uint32_t> StartDependOnMe;
};

class Graph {
	
	private:
	
	std::vector<Node*> RootNodes;
	std::vector<Node*> allNodes;

	//uint32_t num_nodes;
	uint32_t num_edges;
	uint32_t offset_cntr;

	char* mapping_start;

/*
	uint64_t get_file_size(int fd) {
		
		struct stat f_info;
		int r = fstat(fd, &f_info);
		assert(r == 0);
		return f_info.st_size;
	}
*/

	void find_root_nodes() {

		RootNodes.clear();

		for (std::vector<Node*>::iterator it =  allNodes.begin(); it != allNodes.end(); it++) {
			if ((**it).DependenciesCnt == 0) RootNodes.push_back(*it);
		}

	}

	public:

	~Graph() {
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			delete *it;
		}
	}

	Graph() {
	//	num_nodes = 0;
		num_edges = 0;
		offset_cntr = 0;
	}
	
	inline Node* addNode() {
		
		/**
			add a new node to the graph
		*/
		
		Node* N = new Node;
		N->DependenciesCnt = 0;
		N->offset = offset_cntr;
		offset_cntr++;
		allNodes.push_back(N);
		
		return N;
	}

	inline void addDependency(Node* a, Node* b) {
		
		/** 
		   addDependency(a,b) means that a can not be
		   executed before b is finished 
		*/

		b->DependOnMe.push_back(a);
		a->DependenciesCnt++;
		num_edges++;
	}

	inline void addStartDependency(Node* a, Node *b) {
		
		/** 
		   addStartDependency(a,b) means that a can not be
		   executed before b is started
		*/

		b->StartDependOnMe.push_back(a);
		a->DependenciesCnt++;
		num_edges++;
	}

	void write_as_dot() {
		/** 
			Produces a dot representation of the graph. This is usefull for debugging purposes.
		*/
		
		FILE* fd = fopen("graph.dot", "w");
		assert(fd != NULL);
		fprintf(fd, "digraph mygraph {\n");
		fprintf(fd, "graph [rankdir=LR];\n");
		fprintf(fd, "node [shape=record];\n");
					
			for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				char typestr[5];
				if ((**it).Type == OPTYPE_SEND) strcpy(typestr, "Send");
				else if ((**it).Type == OPTYPE_RECV) strcpy(typestr, "Recv");
				else if ((**it).Type == OPTYPE_CALC) strcpy(typestr, "Calc");
				else strcpy(typestr, "Unkn");
				fprintf(fd, "%i [label=\"<f0> Type: %s | <f1> Peer: %i | <f2> Size: %lu | <f3> Tag: %i | <f4> Proc: %i | <f5> Nic: %i \"]\n", 
							 (**it).offset,    typestr,        (**it).Peer, (unsigned long int) (**it).Size,  (**it).Tag  , (**it).Proc,     (**it).Nic);
			}

			for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0\n", (**it).offset, (**dit).offset );
				}
				for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0 [arrowhead=diamond]\n", (**it).offset, (**dit).offset );
				}
			}

		fprintf(fd, "} \n");

	}

/*
	void serialize(FILE* fd, uint32_t rank, uint32_t num_ranks) {

		uint32_t buf32;

		rewind(fd);

		if (rank == 0) {
			// make room for jumptable
			fseek(fd, sizeof(uint64_t)*2*num_ranks + sizeof(uint32_t), SEEK_SET);
		}
		else {
			// jump to the end of the file
			fseek(fd, sizeof(uint32_t) + (2*(rank-1)+1)*sizeof(uint64_t), SEEK_SET);
			uint64_t eos;
			fread(&eos, sizeof(uint64_t), 1, fd);
			fseek(fd, eos, SEEK_SET);
		}
		
		find_root_nodes();
	
		long start = ftell(fd);
		// appendix starts right after all nodes
		long pos_in_appendix = start + 
		                       sizeof(uint32_t)*2 + // for Nodecount and Independent Actions count
							   sizeof(uint32_t)*RootNodes.size() + // for indp. actions offsets
							   (sizeof(char) + sizeof(uint8_t)*2 + sizeof(uint32_t)*7 + sizeof(uint64_t)) * allNodes.size(); // for actual nodeinfo
		// number of elements in the appendix so far
		uint32_t num_in_appendix = 0;

		// number of nodes in the schedule
		buf32 = (uint32_t) allNodes.size();
		fwrite( &buf32, sizeof(uint32_t), 1, fd );


		// write independant actions offsets
		buf32 = RootNodes.size(); 
		fwrite(&buf32, sizeof(uint32_t), 1, fd); // number of independent actions
		for (std::vector<Node*>::iterator it = RootNodes.begin(); it != RootNodes.end(); it++) {
			buf32 = (**it).offset;
			fwrite(&buf32, sizeof(uint32_t), 1, fd); // offset of independent action
		}
		

		Node n;
		int cnt = 0;
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			
			cnt++;

			// write the actual operation info - this is a bit complicated thanks to padding that the compiler might do...
			fwrite( &((**it).DependenciesCnt), sizeof(uint32_t), 1, fd );
			fwrite( &((**it).Type), sizeof(char), 1, fd );
			fwrite( &((**it).Peer), sizeof(uint32_t), 1, fd );
			fwrite( &((**it).Size), sizeof(uint64_t), 1, fd );
			fwrite( &((**it).Tag), sizeof(uint32_t), 1, fd );
			fwrite( &((**it).Proc), sizeof(uint8_t), 1, fd );
			fwrite( &((**it).Nic), sizeof(uint8_t), 1, fd );
			
			// handling dependencies
			buf32 = (**it).DependOnMe.size();
			fwrite( &buf32, sizeof(uint32_t), 1, fd ); // number of ops depending on this node
			
			if ((**it).DependOnMe.size() > 0) {
				fwrite( &num_in_appendix, sizeof(uint32_t), 1, fd ); // start of dependant offsets in appendix
			} 
			else {
				uint32_t b = 999;
				fwrite( &b, sizeof(uint32_t), 1, fd ); // undefined
			}
			
			long pos = ftell(fd);
			// write offsets of depending ops into appendix
			fseek(fd, pos_in_appendix, SEEK_SET);
			for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) {
				fwrite( &((**dit).offset), sizeof(uint32_t), 1, fd );
			}
			// adjust number of elements and position in appendix
			num_in_appendix += (**it).DependOnMe.size();
			pos_in_appendix = ftell(fd);
			// jump back to the node info (we're in appendix right now)
			fseek(fd, pos, SEEK_SET);
			
			// handling start-dependencies
			buf32 = (**it).StartDependOnMe.size();
			fwrite( &buf32, sizeof(uint32_t), 1, fd ); // number of ops start-depending on this node
			if ((**it).StartDependOnMe.size() > 0) { 
				fwrite( &num_in_appendix, sizeof(uint32_t), 1, fd ); // start of start-dependant offsets in appendix
			}
			else {
				uint32_t b = -1;
				fwrite( &b, sizeof(uint32_t), 1, fd ); // undefinedx
			}
			pos =  ftell(fd);
			// write offsets of start-depending ops into appendix
			fseek(fd, pos_in_appendix, SEEK_SET);
			for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) {
				fwrite( &((**dit).offset), sizeof(uint32_t), 1, fd );
			}
			// adjust number of elements and position in appendix
			num_in_appendix += (**it).StartDependOnMe.size();
			pos_in_appendix = ftell(fd);
			// jump back to the node info (we're in appendix right now)
			fseek(fd, pos, SEEK_SET);
		
		}
		
		// write jumptable info: the schedule started at start and ended at pos_in_appendix
		fseek(fd, 0, SEEK_SET);
		
		// the number of ranks in this file
		buf32 = num_ranks;
		fwrite(&buf32, sizeof(uint32_t), 1, fd);
		fseek(fd, sizeof(uint64_t)*2*rank + sizeof(uint32_t), SEEK_SET);
		uint64_t buf64 = start;
		fwrite(&buf64, sizeof(uint64_t), 1, fd);
		buf64 = pos_in_appendix;
		fwrite(&buf64, sizeof(uint64_t), 1, fd);
	}

*/

	void serialize_mmap(int fd, uint32_t rank, uint32_t num_ranks, uint8_t max_cpu, uint8_t max_nic) {
		
		char *start_rankdata;
		uint64_t end_of_lastrank;
		static uint64_t filesize;
		char *pos;

		find_root_nodes();

		if (rank == 0) {
			// calculate the size of the file
			filesize = 0;
			filesize += sizeof(uint64_t); // magic cookie
			filesize += sizeof(uint32_t); // num ranks
			filesize += sizeof(uint8_t); // max_cpu
			filesize += sizeof(uint8_t); // max_nic
			filesize += sizeof(uint64_t)*2*num_ranks; // jumptable
			filesize += sizeof(uint32_t); // num nodes
			filesize += sizeof(uint32_t); // num indp actions
			filesize += (sizeof(uint32_t)*RootNodes.size()); // rootnodes offsets
			filesize += (sizeof(char)+sizeof(uint8_t)*2+sizeof(uint32_t)*7+sizeof(uint64_t))*allNodes.size(); // nodeinfo
			filesize += (sizeof(uint32_t)*num_edges); //appendix

			// enlarge the file
			lseek(fd, filesize-1, SEEK_SET);
			int r = write(fd, "", 1);
			assert(r == 1);
			
			// mmap the file
			mapping_start = (char*) mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (mapping_start == MAP_FAILED) {
				perror("couldn't mmap the output file");
				exit(EXIT_FAILURE);
			}
			goal_binary_detail::store_packed(
				mapping_start, static_cast<uint64_t>(MAGIC_COOKIE));
			mapping_start += sizeof(uint64_t); // jump over magic cookie
			end_of_lastrank = sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*num_ranks;
			start_rankdata = mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*num_ranks;  // our rankdata starts right after the jumptable
			
		}
		else {
			filesize += sizeof(uint32_t); // num nodes
			filesize += sizeof(uint32_t); // num indp actions
			filesize += sizeof(uint32_t)*RootNodes.size(); // rootnodes offsets
			filesize += (sizeof(char)+sizeof(uint8_t)*2+sizeof(uint32_t)*7+sizeof(uint64_t))*allNodes.size(); // nodeinfo
			filesize += sizeof(uint32_t)*num_edges; //appendix


			// enlarge the file
			lseek(fd, filesize-1, SEEK_SET);
			int r = write(fd, "", 1);
			assert(r == 1);

			// mmap the file
			mapping_start = (char*) mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (mapping_start == MAP_FAILED) {
				perror("mmap failed");
				exit(EXIT_FAILURE);
			}

			mapping_start += sizeof(uint64_t); // jump over magic cookie

			end_of_lastrank = goal_binary_detail::load_packed<uint64_t>(
				mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2
				+ sizeof(uint64_t)*(2*(rank-1)+1));
			start_rankdata = mapping_start + end_of_lastrank;
		}

		pos = start_rankdata;
		uint32_t num_in_appendix = 0;

		{
			const uint32_t size = (uint32_t) allNodes.size();
			memcpy(pos, &size, sizeof(uint32_t));	 	pos += sizeof(uint32_t);	// number of nodes in the schedule
		}
		{
			const uint32_t size = (uint32_t) RootNodes.size();
			memcpy(pos, &size, sizeof(uint32_t));		pos += sizeof(uint32_t);	// number of independent actions
		}
		
		// independent action offsets
		for (std::vector<Node*>::iterator it = RootNodes.begin(); it != RootNodes.end(); it++) {
			memcpy(pos, &(**it).offset, sizeof(uint32_t)); pos += sizeof(uint32_t);
		}

		// node data 

		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {

			memcpy(pos, &(**it).DependenciesCnt, sizeof(uint32_t)); 		pos += sizeof(uint32_t);	// number of actions this action depends on
			memcpy(pos, &(**it).Type, sizeof(char));                		pos += sizeof(char);		// type of this action
			memcpy(pos, &(**it).Peer, sizeof(uint32_t));            		pos += sizeof(uint32_t);	// peer of this action
			memcpy(pos, &(**it).Size, sizeof(uint64_t));            		pos += sizeof(uint64_t);	// size of the data transfer / length of the local calculation
			memcpy(pos, &(**it).Tag, sizeof(uint32_t));             		pos += sizeof(uint32_t);	// tag of the send/recv operation
			memcpy(pos, &(**it).Proc, sizeof(uint8_t));             		pos += sizeof(uint8_t);		// processor used for this action
			memcpy(pos, &(**it).Nic, sizeof(uint8_t));              		pos += sizeof(uint8_t);		// network interface used for this action
			{
				const uint32_t size = (**it).DependOnMe.size();
				memcpy(pos, &size, sizeof(uint32_t));                 		pos += sizeof(uint32_t);	// number of actions that depend on this actions termination
			}
			memcpy(pos, &num_in_appendix, sizeof(uint32_t));    				pos += sizeof(uint32_t);	// start index of dependent actions (in appendix)
			num_in_appendix += (**it).DependOnMe.size();
			{
				const uint32_t size = (**it).StartDependOnMe.size();
				memcpy(pos, &size, sizeof(uint32_t));                   	pos += sizeof(uint32_t);	// number of actions that depend on this actions start
			}
			memcpy(pos, &num_in_appendix, sizeof(uint32_t));    				pos += sizeof(uint32_t);	// start index of start-dependent actions (in appendix)
			num_in_appendix += (**it).StartDependOnMe.size();
		}
		
		// appendix data
	
		for (std::vector<Node*>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
			for (std::vector<Node*>::iterator dit = (**it).DependOnMe.begin(); dit != (**it).DependOnMe.end(); dit++) {
				memcpy(pos, &(**dit).offset, sizeof(uint32_t));       		pos += sizeof(uint32_t);	// offset of dependent action
			}
			for (std::vector<Node*>::iterator dit = (**it).StartDependOnMe.begin(); dit != (**it).StartDependOnMe.end(); dit++) {
				memcpy(pos, &(**dit).offset, sizeof(uint32_t));       		pos += sizeof(uint32_t);	// offset of start-dependent action
			}
		}

		// jumptable info
		goal_binary_detail::store_packed(mapping_start, num_ranks);								// number of ranks in this schedule-file

		goal_binary_detail::store_packed(
			mapping_start + sizeof(uint32_t), max_cpu);	// minimal number of cpu required to simulate this schedule
		goal_binary_detail::store_packed(
			mapping_start + sizeof(uint32_t) + sizeof(uint8_t), max_nic);	// minimal number of nics required to simulate this schedule
		
		goal_binary_detail::store_packed(
			mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2
			+ sizeof(uint64_t)*2*rank,
			end_of_lastrank);	// start of this ranks info
		goal_binary_detail::store_packed(
			mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2
			+ sizeof(uint64_t)*(2*rank+1),
			static_cast<uint64_t>(pos - mapping_start));	// end of this ranks info
	
		//printf("s: %llu e: %llu\n", (long long unsigned int) end_of_lastrank, (long long unsigned int) (pos - mapping_start));

		// munmap the files so that the contents get written
		int r = munmap(
			mapping_start - sizeof(uint64_t),
			static_cast<size_t>(filesize));
		assert(r == 0);	
	}

};

class SerializedGraph {
	
	private:
	
	static constexpr size_t NODE_INFO_BYTES =
		sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*7
		+ sizeof(uint8_t)*2;
	static constexpr size_t RANK_COUNTS_BYTES = sizeof(uint32_t)*2;
	static constexpr size_t NODE_TYPE_OFFSET = sizeof(uint32_t);
	static constexpr size_t NODE_PEER_OFFSET =
		sizeof(uint32_t) + sizeof(char);
	static constexpr size_t NODE_SIZE_OFFSET =
		NODE_PEER_OFFSET + sizeof(uint32_t);
	static constexpr size_t NODE_TAG_OFFSET =
		NODE_SIZE_OFFSET + sizeof(uint64_t);
	static constexpr size_t NODE_PROC_OFFSET =
		NODE_TAG_OFFSET + sizeof(uint32_t);
	static constexpr size_t NODE_NIC_OFFSET =
		NODE_PROC_OFFSET + sizeof(uint8_t);
	static constexpr size_t NODE_DEP_COUNT_OFFSET =
		NODE_NIC_OFFSET + sizeof(uint8_t);
	static constexpr size_t NODE_DEP_START_OFFSET =
		NODE_DEP_COUNT_OFFSET + sizeof(uint32_t);
	static constexpr size_t NODE_START_DEP_COUNT_OFFSET =
		NODE_DEP_START_OFFSET + sizeof(uint32_t);
	static constexpr size_t NODE_START_DEP_START_OFFSET =
		NODE_START_DEP_COUNT_OFFSET + sizeof(uint32_t);

	char* mapping_start;
	size_t mapping_length;
	size_t node_table_offset;
	size_t appendix_offset;
	size_t appendix_entries;
	uint32_t num_root_nodes;
	uint32_t num_nodes;
	uint32_t num_ranks_in_schedule;

	std::vector<DeserializedNode> executableNodes;

	// A hashmap that maps the offset of a node to the timestamp
	// at which it should start. This is updated every time
	// `MarkNodeAsStarted()` is called. In `MarkNodeAsDone()`,
	// a timestamp is passed to the function, which is then
	// used to update the `node_start_time` hashmap.
	std::map<uint32_t, uint64_t> node_start_time;
	template <typename T>
	T load_rank(size_t offset, const char* field) const {
		return goal_binary_detail::load_bounded<T>(
			mapping_start, mapping_length, offset, field);
	}

	template <typename T>
	void store_rank(size_t offset, const T& value, const char* field) {
		goal_binary_detail::store_bounded(
			mapping_start, mapping_length, offset, value, field);
	}

	size_t node_offset(uint32_t offset) const {
		if (offset >= num_nodes) {
			throw std::out_of_range(
				"serialized GOAL node offset is outside the rank");
		}
		return goal_binary_detail::checked_add(
			node_table_offset,
			goal_binary_detail::checked_multiply(
				static_cast<size_t>(offset), NODE_INFO_BYTES,
				"node table"),
			"node table");
	}

	template <typename T>
	T load_node(uint32_t node, size_t field_offset, const char* field) const {
		return load_rank<T>(
			goal_binary_detail::checked_add(
				node_offset(node), field_offset, field),
			field);
	}

	uint32_t load_appendix(size_t index) const {
		if (index >= appendix_entries) {
			throw goal_binary_detail::malformed(
				"dependency appendix index is outside the rank");
		}
		return load_rank<uint32_t>(
			goal_binary_detail::checked_add(
				appendix_offset,
				goal_binary_detail::checked_multiply(
					index, sizeof(uint32_t), "dependency appendix"),
				"dependency appendix"),
			"dependency appendix entry");
	}

	void validate_dependency_range(
			uint32_t count, uint32_t start, const char* field) const {
		if (count == 0) {
			return;
		}
		const size_t checked_start = static_cast<size_t>(start);
		const size_t checked_count = static_cast<size_t>(count);
		if (checked_start > appendix_entries
				|| checked_count > appendix_entries - checked_start) {
			throw goal_binary_detail::malformed(
				std::string(field) + " range is outside the appendix");
		}
	}

	void validate_layout() {
		if (num_root_nodes > num_nodes) {
			throw goal_binary_detail::malformed(
				"root-node count exceeds node count");
		}

		const size_t root_bytes = goal_binary_detail::checked_multiply(
			static_cast<size_t>(num_root_nodes), sizeof(uint32_t),
			"root table");
		node_table_offset = goal_binary_detail::checked_add(
			RANK_COUNTS_BYTES, root_bytes, "root table");
		const size_t node_bytes = goal_binary_detail::checked_multiply(
			static_cast<size_t>(num_nodes), NODE_INFO_BYTES, "node table");
		appendix_offset = goal_binary_detail::checked_add(
			node_table_offset, node_bytes, "node table");
		if (appendix_offset > mapping_length) {
			throw goal_binary_detail::malformed(
				"rank ends inside its node table");
		}
		const size_t appendix_bytes = mapping_length - appendix_offset;
		if (appendix_bytes % sizeof(uint32_t) != 0) {
			throw goal_binary_detail::malformed(
				"dependency appendix has a partial entry");
		}
		appendix_entries = appendix_bytes / sizeof(uint32_t);

		std::vector<bool> roots(num_nodes, false);
		for (uint32_t index = 0; index < num_root_nodes; ++index) {
			const uint32_t root = load_rank<uint32_t>(
				RANK_COUNTS_BYTES + static_cast<size_t>(index)*sizeof(uint32_t),
				"root-node id");
			if (root >= num_nodes) {
				throw goal_binary_detail::malformed(
					"root-node id is outside the rank");
			}
			if (roots[root]) {
				throw goal_binary_detail::malformed(
					"root-node table contains a duplicate");
			}
			roots[root] = true;
		}

		std::vector<uint32_t> expected_dependencies(num_nodes, 0);
		for (uint32_t node = 0; node < num_nodes; ++node) {
			const char type = load_node<char>(
				node, NODE_TYPE_OFFSET, "node type");
			if (type != OPTYPE_SEND && type != OPTYPE_RECV
					&& type != OPTYPE_CALC) {
				throw goal_binary_detail::malformed("node has an unknown type");
			}
			const uint32_t dependency_count = load_node<uint32_t>(
				node, NODE_DEP_COUNT_OFFSET, "dependency count");
			const uint32_t dependency_start = load_node<uint32_t>(
				node, NODE_DEP_START_OFFSET, "dependency start");
			const uint32_t start_dependency_count = load_node<uint32_t>(
				node, NODE_START_DEP_COUNT_OFFSET,
				"start-dependency count");
			const uint32_t start_dependency_start = load_node<uint32_t>(
				node, NODE_START_DEP_START_OFFSET,
				"start-dependency start");
			validate_dependency_range(
				dependency_count, dependency_start, "dependency");
			validate_dependency_range(
				start_dependency_count, start_dependency_start,
				"start-dependency");

			const auto count_incoming = [&](uint32_t target) {
				if (target >= num_nodes) {
					throw goal_binary_detail::malformed(
						"dependency node id is outside the rank");
				}
				if (expected_dependencies[target]
						== std::numeric_limits<uint32_t>::max()) {
					throw goal_binary_detail::malformed(
						"dependency count overflows");
				}
				++expected_dependencies[target];
			};
			for (size_t index = 0; index < dependency_count; ++index) {
				count_incoming(load_appendix(
					static_cast<size_t>(dependency_start) + index));
			}
			for (size_t index = 0; index < start_dependency_count; ++index) {
				count_incoming(load_appendix(
					static_cast<size_t>(start_dependency_start) + index));
			}
		}

		for (uint32_t node = 0; node < num_nodes; ++node) {
			const uint32_t stored_dependencies = load_node<uint32_t>(
				node, 0, "stored dependency count");
			if (stored_dependencies != expected_dependencies[node]) {
				throw goal_binary_detail::malformed(
					"stored dependency count does not match the appendix");
			}
			if (roots[node] != (stored_dependencies == 0)) {
				throw goal_binary_detail::malformed(
					"root-node table does not match dependency counts");
			}
		}
	}

	std::vector<uint32_t> decrement_dependencies(
			const std::vector<uint32_t>& offsets) {
		std::map<uint32_t, uint32_t> occurrences;
		for (const uint32_t offset : offsets) {
			if (offset >= num_nodes
					|| occurrences[offset] == std::numeric_limits<uint32_t>::max()) {
				throw std::logic_error("invalid GOAL dependency update");
			}
			++occurrences[offset];
		}

		std::map<uint32_t, uint32_t> remaining;
		for (const auto& entry : occurrences) {
			const uint32_t current = load_rank<uint32_t>(
				node_offset(entry.first), "mutable dependency count");
			if (current < entry.second) {
				throw std::logic_error(
					"serialized GOAL dependency count would underflow");
			}
			remaining.emplace(entry.first, current - entry.second);
		}
		for (const auto& entry : remaining) {
			store_rank(
				node_offset(entry.first), entry.second,
				"mutable dependency count");
		}

		std::vector<uint32_t> unlocked;
		for (const uint32_t offset : offsets) {
			const auto entry = remaining.find(offset);
			if (entry != remaining.end() && entry->second == 0) {
				unlocked.push_back(offset);
				remaining.erase(entry);
			}
		}
		return unlocked;
	}

	void add_root_nodes() {
		for (uint32_t index = 0; index < num_root_nodes; ++index) {
			const uint32_t offset = load_rank<uint32_t>(
				RANK_COUNTS_BYTES + static_cast<size_t>(index)*sizeof(uint32_t),
				"root-node id");
			executableNodes.push_back(get_node_by_offset(offset));
		}
	}

	DeserializedNode get_node_by_offset(uint32_t offset) {
		if (offset >= num_nodes) {
			throw std::out_of_range(
				"serialized GOAL node offset is outside the rank");
		}
		DeserializedNode node{};
		node.DependenciesCnt = load_node<uint32_t>(
			offset, 0, "stored dependency count");
		node.Type = load_node<char>(offset, NODE_TYPE_OFFSET, "node type");
		node.Peer = load_node<uint32_t>(
			offset, NODE_PEER_OFFSET, "node peer");
		node.Size = load_node<uint64_t>(
			offset, NODE_SIZE_OFFSET, "node size");
		node.Tag = load_node<uint32_t>(
			offset, NODE_TAG_OFFSET, "node tag");
		node.Proc = load_node<uint8_t>(
			offset, NODE_PROC_OFFSET, "node processor");
		node.Nic = load_node<uint8_t>(
			offset, NODE_NIC_OFFSET, "node NIC");
		node.offset = offset;
		node.start_time = 0;

		const uint32_t dependency_count = load_node<uint32_t>(
			offset, NODE_DEP_COUNT_OFFSET, "dependency count");
		const uint32_t dependency_start = load_node<uint32_t>(
			offset, NODE_DEP_START_OFFSET, "dependency start");
		const uint32_t start_dependency_count = load_node<uint32_t>(
			offset, NODE_START_DEP_COUNT_OFFSET, "start-dependency count");
		const uint32_t start_dependency_start = load_node<uint32_t>(
			offset, NODE_START_DEP_START_OFFSET, "start-dependency start");
		node.DependOnMe.reserve(dependency_count);
		node.StartDependOnMe.reserve(start_dependency_count);
		for (size_t index = 0; index < dependency_count; ++index) {
			node.DependOnMe.push_back(load_appendix(
				static_cast<size_t>(dependency_start) + index));
		}
		for (size_t index = 0; index < start_dependency_count; ++index) {
			node.StartDependOnMe.push_back(load_appendix(
				static_cast<size_t>(start_dependency_start) + index));
		}
		return node;
	}


	public:

	uint32_t GetNumNodes() {
		return this->num_nodes;		
	}

	void write_as_dot(char *filename) {
		/** 
			Produces a dot representation of the graph. This is usefull for debugging purposes.
		*/
		
		std::vector<DeserializedNode> allNodes;
		std::vector<DeserializedNode> executableNodes;

		executableNodes = GetExecutableNodes_DSN();
		while (executableNodes.size() > 0) {
			for (uint32_t cnt=0; cnt < executableNodes.size(); cnt++) {
				allNodes.push_back(executableNodes[cnt]);
				MarkNodeAsStarted_DSN(executableNodes[cnt]);
				MarkNodeAsDone_DSN(executableNodes[cnt]);
			}
			executableNodes.clear();
			executableNodes = GetExecutableNodes_DSN();
		}

		FILE* fd = fopen(filename, "w");
		assert(fd != NULL);
		fprintf(fd, "digraph mygraph {\n");
		fprintf(fd, "graph [rankdir=LR];\n");
		fprintf(fd, "node [shape=record];\n");
					
			for (std::vector<DeserializedNode>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				char typestr[5];
				if ((*it).Type == OPTYPE_SEND) strcpy(typestr, "Send");
				else if ((*it).Type == OPTYPE_RECV) strcpy(typestr, "Recv");
				else if ((*it).Type == OPTYPE_CALC) strcpy(typestr, "Calc");
				else strcpy(typestr, "Unkn");
				fprintf(fd, "%i [label=\"<f0> Type: %s | <f1> Peer: %i | <f2> Size: %llu | <f3> Tag: %i | <f4> Proc: %i | <f5> Nic: %i \"]\n", 
							 (*it).offset,    typestr,        (*it).Peer, (unsigned long long) (*it).Size,  (*it).Tag  , (*it).Proc,     (*it).Nic);
			}

			for (std::vector<DeserializedNode>::iterator it = allNodes.begin(); it != allNodes.end(); it++) {
				for (std::vector<uint32_t>::iterator dit = (*it).DependOnMe.begin(); dit != (*it).DependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0\n", (*it).offset, (*dit));
				}
				for (std::vector<uint32_t>::iterator dit = (*it).StartDependOnMe.begin(); dit != (*it).StartDependOnMe.end(); dit++) {
					fprintf(fd, "%i:f0 -> %i:f0 [arrowhead=diamond]\n", (*it).offset, (*dit));
				}
			}

		fprintf(fd, "} \n");

	}

	SerializedGraph(char* map_start, size_t map_length, uint32_t rank)
			: mapping_start(nullptr),
			  mapping_length(0),
			  node_table_offset(0),
			  appendix_offset(0),
			  appendix_entries(0),
			  num_root_nodes(0),
			  num_nodes(0),
			  num_ranks_in_schedule(0) {
		constexpr size_t schedule_header_prefix =
			sizeof(uint32_t) + sizeof(uint8_t)*2;
		constexpr size_t jump_entry_bytes = sizeof(uint64_t)*2;
		if (map_start == nullptr) {
			throw goal_binary_detail::malformed("schedule mapping is null");
		}
		num_ranks_in_schedule = goal_binary_detail::load_bounded<uint32_t>(
			map_start, map_length, 0, "rank count");
		if (num_ranks_in_schedule == 0) {
			throw goal_binary_detail::malformed("rank count is zero");
		}
		if (rank >= num_ranks_in_schedule) {
			throw goal_binary_detail::malformed(
				"requested rank is outside the jump table");
		}
		const size_t jump_table_bytes = goal_binary_detail::checked_multiply(
			static_cast<size_t>(num_ranks_in_schedule), jump_entry_bytes,
			"rank jump table");
		const size_t header_bytes = goal_binary_detail::checked_add(
			schedule_header_prefix, jump_table_bytes, "rank jump table");
		if (header_bytes > map_length) {
			throw goal_binary_detail::malformed("rank jump table is truncated");
		}
		const size_t entry_offset = goal_binary_detail::checked_add(
			schedule_header_prefix,
			goal_binary_detail::checked_multiply(
				static_cast<size_t>(rank), jump_entry_bytes,
				"rank jump table"),
			"rank jump table");
		const uint64_t rank_start_u64 =
			goal_binary_detail::load_bounded<uint64_t>(
				map_start, map_length, entry_offset, "rank start");
		const uint64_t rank_end_u64 =
			goal_binary_detail::load_bounded<uint64_t>(
				map_start, map_length, entry_offset + sizeof(uint64_t),
				"rank end");
		if (rank_start_u64 > std::numeric_limits<size_t>::max()
				|| rank_end_u64 > std::numeric_limits<size_t>::max()) {
			throw goal_binary_detail::malformed(
				"rank span does not fit the host address space");
		}
		const size_t rank_start = static_cast<size_t>(rank_start_u64);
		const size_t rank_end = static_cast<size_t>(rank_end_u64);
		if (rank_start < header_bytes || rank_start > rank_end
				|| rank_end > map_length) {
			throw goal_binary_detail::malformed(
				"rank span is outside the schedule mapping");
		}
		mapping_start = map_start + rank_start;
		mapping_length = rank_end - rank_start;
		if (mapping_length < RANK_COUNTS_BYTES) {
			throw goal_binary_detail::malformed("rank header is truncated");
		}
		num_nodes = load_rank<uint32_t>(0, "node count");
		num_root_nodes = load_rank<uint32_t>(
			sizeof(uint32_t), "root-node count");
		validate_layout();
		add_root_nodes();
	}

	void MarkNodeAsStarted_DSN(DeserializedNode node) {

		DeserializedNode N = get_node_by_offset(node.offset);
		for (const uint32_t offset :
				decrement_dependencies(N.StartDependOnMe)) {
			executableNodes.push_back(get_node_by_offset(offset));
		}
	}
	
	void MarkNodeAsDone_DSN(DeserializedNode node)
	{
		
		DeserializedNode N = get_node_by_offset(node.offset);
		for (const uint32_t offset : decrement_dependencies(N.DependOnMe)) {
			executableNodes.push_back(get_node_by_offset(offset));
		}
	}
	
	std::vector<DeserializedNode> GetExecutableNodes_DSN() { 
				
		std::vector<DeserializedNode> ret;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {	
			ret.push_back(executableNodes[cnt]);
		}
		executableNodes.clear();
		return ret;
	}

  typedef std::vector<graph_node_properties> nodelist_t;
	void GetExecutableNodes(nodelist_t *ret_ptr) {
		nodelist_t& ret = *ret_ptr;

		for (uint32_t cnt=0; cnt<executableNodes.size(); cnt++) {	
			graph_node_properties gp{};
			gp.target = executableNodes[cnt].Peer;
			gp.size = executableNodes[cnt].Size;
			gp.tag = executableNodes[cnt].Tag;
			gp.proc = executableNodes[cnt].Proc;
			gp.nic = executableNodes[cnt].Nic;
			gp.starttime = executableNodes[cnt].start_time;
			if (executableNodes[cnt].Type == OPTYPE_SEND) gp.type = OP_SEND;
			else if (executableNodes[cnt].Type == OPTYPE_RECV) gp.type = OP_RECV;
			else if (executableNodes[cnt].Type == OPTYPE_CALC) gp.type = OP_LOCOP;
			else throw std::logic_error("validated GOAL node has an unknown type");
			gp.offset = executableNodes[cnt].offset;
			ret.push_back(gp);
		}
		executableNodes.clear();
	}

	void MarkNodeAsStarted(uint32_t offset) {

		DeserializedNode N = get_node_by_offset(offset);
		for (const uint32_t unlocked_offset :
				decrement_dependencies(N.StartDependOnMe)) {
			DeserializedNode freed = get_node_by_offset(unlocked_offset);
			freed.start_time = 0;
			executableNodes.push_back(freed);
		}
	}


	void MarkNodeAsDone(uint32_t offset) {

        DeserializedNode N = get_node_by_offset(offset);

        /* if (N.Type == OP_LOCOP_IN_PROGRESS || N.Type == OP_LOCOP) {
            printf("Size depending on me %d - Rank %d Offset %d\n",
                   N.DependOnMe.size(), my_rank, offset);
        } */
	        for (const uint32_t unlocked_offset :
	                decrement_dependencies(N.DependOnMe)) {
	            executableNodes.push_back(get_node_by_offset(unlocked_offset));
	        }
    }
	
	/**
	 * Mark a node as done. This means that all nodes that depend on this node which
	 * do not have any other dependencies can be executed now.
	 * @param offset The offset of the node that is done
	 * @param cpu_time The timestamp of the CPU that executed the node specified
	 * by the offset
	 */
	void MarkNodeAsDone(uint32_t offset, uint64_t cpu_time)
	{
		DeserializedNode N = get_node_by_offset(offset);
		//std::cout << "[INFO] " << "Host: " << my_rank << ", Node " << offset << " has " << N.DependOnMe.size() << " dependencies" << std::endl;
		//printf("Executable1 Nodes: %u\n", executableNodes.size());
		const std::vector<uint32_t> unlocked =
			decrement_dependencies(N.DependOnMe);
		for (const uint32_t offset : N.DependOnMe) {
			// Checks if offset is in node_start_time
			if (node_start_time.find(offset) == node_start_time.end())
			{
				node_start_time[offset] = cpu_time;
			}
			else
			{
				node_start_time[offset] = std::max(node_start_time[offset], cpu_time);
			}			
		}
		for (const uint32_t offset : unlocked) {
			DeserializedNode freed = get_node_by_offset(offset);
			freed.start_time = node_start_time[offset];
			node_start_time.erase(offset);
			executableNodes.push_back(freed);
		}
		//printf("[%d-%d] Unlocked Number Nodes: %u\n", my_rank, offset, executableNodes.size());
	}

};

class Parser {

	private:

	char* mapping_start;
	size_t mapping_length;
	uint32_t num_ranks_in_schedule;
	uint8_t max_cpu;
	uint8_t max_nic;
	FILE *schedules_fd;

	uint64_t get_file_size(FILE* fd) {
		struct stat f_info;
		if (fstat(fileno(fd), &f_info) != 0 || f_info.st_size < 0) {
			throw std::runtime_error(
				"could not determine serialized GOAL schedule size");
		}
		return static_cast<uint64_t>(f_info.st_size);
	}

	void release_mapping_and_file() noexcept {
		if (mapping_start != nullptr && mapping_start != MAP_FAILED) {
			(void)munmap(mapping_start, mapping_length);
		}
		mapping_start = nullptr;
		if (schedules_fd != nullptr) {
			(void)fclose(schedules_fd);
		}
		schedules_fd = nullptr;
	}

	public:
	
  typedef std::vector<SerializedGraph> schedules_t ;
	schedules_t schedules;

	Parser(std::string filename, bool save_mem)
			: mapping_start(nullptr),
			  mapping_length(0),
			  num_ranks_in_schedule(0),
			  max_cpu(0),
			  max_nic(0),
			  schedules_fd(nullptr) {
		try {
			schedules_fd = fopen(filename.c_str(), "r+b");
			if (schedules_fd == nullptr) {
				throw std::runtime_error(
					"could not open serialized GOAL schedule " + filename);
			}

			const uint64_t file_size = get_file_size(schedules_fd);
			if (file_size > std::numeric_limits<size_t>::max()) {
				throw goal_binary_detail::malformed(
					"file does not fit the host address space");
			}
			mapping_length = static_cast<size_t>(file_size);
			constexpr size_t fixed_file_header =
				sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint8_t)*2;
			if (mapping_length < fixed_file_header) {
				throw goal_binary_detail::malformed("file header is truncated");
			}

			uint64_t magic_cookie = 0;
			if (fread(&magic_cookie, sizeof(uint64_t), 1, schedules_fd) != 1
					|| fread(&num_ranks_in_schedule, sizeof(uint32_t), 1,
					         schedules_fd) != 1
					|| fread(&max_cpu, sizeof(uint8_t), 1, schedules_fd) != 1
					|| fread(&max_nic, sizeof(uint8_t), 1, schedules_fd) != 1) {
				throw goal_binary_detail::malformed("file header is truncated");
			}
			if (magic_cookie == MAGIC_COOKIE_INVALID) {
				throw goal_binary_detail::malformed(
					"schedule was invalidated by a prior shared-memory run");
			}
			if (magic_cookie != MAGIC_COOKIE) {
				throw goal_binary_detail::malformed("magic cookie is missing");
			}
			if (num_ranks_in_schedule == 0) {
				throw goal_binary_detail::malformed("rank count is zero");
			}

			const size_t schedule_bytes = mapping_length - sizeof(uint64_t);
			const size_t jump_table_bytes = goal_binary_detail::checked_multiply(
				static_cast<size_t>(num_ranks_in_schedule),
				sizeof(uint64_t)*2, "rank jump table");
			const size_t schedule_header_bytes = goal_binary_detail::checked_add(
				sizeof(uint32_t) + sizeof(uint8_t)*2,
				jump_table_bytes, "rank jump table");
			if (schedule_header_bytes > schedule_bytes) {
				throw goal_binary_detail::malformed("rank jump table is truncated");
			}

			const int mmap_flags = save_mem ? MAP_SHARED : MAP_PRIVATE;
			mapping_start = static_cast<char*>(mmap(
				nullptr, mapping_length, PROT_READ | PROT_WRITE,
				mmap_flags, fileno(schedules_fd), 0));
			if (mapping_start == MAP_FAILED) {
				mapping_start = nullptr;
				throw std::runtime_error(
					"could not mmap serialized GOAL schedule " + filename);
			}
			if (save_mem) {
				printf("The schedule will be invalid after this simulation!\n");
			}

			const char* schedule_start = mapping_start + sizeof(uint64_t);
			size_t previous_rank_end = schedule_header_bytes;
			for (uint32_t rank = 0; rank < num_ranks_in_schedule; ++rank) {
				const size_t entry_offset =
					sizeof(uint32_t) + sizeof(uint8_t)*2
					+ static_cast<size_t>(rank)*sizeof(uint64_t)*2;
				const uint64_t rank_start_u64 =
					goal_binary_detail::load_bounded<uint64_t>(
						schedule_start, schedule_bytes, entry_offset,
						"rank start");
				const uint64_t rank_end_u64 =
					goal_binary_detail::load_bounded<uint64_t>(
						schedule_start, schedule_bytes,
						entry_offset + sizeof(uint64_t), "rank end");
				if (rank_start_u64 > std::numeric_limits<size_t>::max()
						|| rank_end_u64 > std::numeric_limits<size_t>::max()) {
					throw goal_binary_detail::malformed(
						"rank span does not fit the host address space");
				}
				const size_t rank_start = static_cast<size_t>(rank_start_u64);
				const size_t rank_end = static_cast<size_t>(rank_end_u64);
				if (rank_start < previous_rank_end || rank_start > rank_end
						|| rank_end > schedule_bytes) {
					throw goal_binary_detail::malformed(
						"rank spans overlap or leave the schedule mapping");
				}
				previous_rank_end = rank_end;
			}

			schedules.reserve(num_ranks_in_schedule);
			for (uint32_t rank = 0; rank < num_ranks_in_schedule; ++rank) {
				schedules.emplace_back(
					mapping_start + sizeof(uint64_t), schedule_bytes, rank);
			}
		} catch (...) {
			release_mapping_and_file();
			throw;
		}
	}

	Parser(const Parser&) = delete;
	Parser& operator=(const Parser&) = delete;

	uint32_t GetNumCPU() {
		return static_cast<uint32_t>(max_cpu) + 1;
	}

	uint32_t GetNumNIC() {
		return static_cast<uint32_t>(max_nic) + 1;
	}


	~Parser() {
		release_mapping_and_file();
	}
	
};
