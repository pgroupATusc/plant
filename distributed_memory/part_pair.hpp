// This file contains implementation of QDOL data layout
// for efficient distributed querying.
//
//  Author: Qing Dong, Kartik Lakhotia
//  Email id: qingdong@usc.edu, klakhoti@usc.edu


#include <vector>
#include <omp.h>
#include <cmath>
#include "graph.hpp"
#include "labeling.hpp"
#include "query_funcs.hpp"
#include <stdlib.h>
#include <mpi.h>

namespace hl {

class PartPair{
	unsigned num_parts;
	unsigned part_size;
	unsigned world_size;
	unsigned world_rank;
	unsigned NUM_THREAD;
	std::vector< std::vector<int> > query2node;
	std::vector< std::pair<Vertex, Vertex> > node2part;
	Labeling &curr_labeling;
	Labeling final_labeling;

public:
	unsigned n;
	PartPair(Labeling & curr_labeling, unsigned world_size, unsigned world_rank, unsigned NUM_THREAD):
		world_size(world_size),
		world_rank(world_rank),
		NUM_THREAD(NUM_THREAD),
		curr_labeling(curr_labeling),
		final_labeling(curr_labeling.n),
		n (curr_labeling.n) 
		{
			//std::cout<<world_rank<<"curr avg_size: "<<curr_labeling.get_avg()<<std::endl;
			distAllPartitions();
			//std::cout<<world_rank<<" final avg_size: "<<final_labeling.get_avg()<<std::endl;
			//std::cout<<world_rank<<" curr avg_size in the end: "<<curr_labeling.get_avg()<<std::endl;
		}

	unsigned vertex2part(Vertex v) 
	{ 
		return v/part_size;
	}	
// distribute global labels from one partition on each machine to those who need it //
	void distPartition (unsigned int pid)
	{
	    Vertex start = pid*part_size;
	    Vertex end = std::min((pid+1)*part_size, n);
        
        Vertex single_start = start;
        Vertex single_end = start;
        std::vector<int> size_per_vertex = gather_size(curr_labeling, start, end, NUM_THREAD);
//	    if(world_rank==0)	std::cout<<"gather size "<<pid<<" success"<<std::endl;
        int MPI_BUDGET = 650000000;
        do {
            single_end = findLastSend(MPI_BUDGET, size_per_vertex, single_start, start, end, NUM_THREAD);
//	        if(world_rank==0)	std::cout<<"one-time comm range: "<<single_start<<"-"<<single_end<<" partition range"<<start<<"-"<<end<<std::endl;
	        std::vector<int>label_list;
	        std::vector<int>recv_buffer;
	        parallelLoad (label_list, curr_labeling, single_start, single_end, NUM_THREAD);
//	        if(world_rank==0)	std::cout<<"load partition "<<pid<<" success"<<std::endl;
	        gatherAllLabelsQuery(label_list, recv_buffer, world_size, world_rank);
//	        if(world_rank==0)	std::cout<<"gather partition "<<pid<<" success"<<std::endl;
	        if (pid==node2part[world_rank].first || pid==node2part[world_rank].second) {
	        	loadFromRecvBuffer(recv_buffer, final_labeling);
//	    	if (world_rank==0) std::cout<<"distribute partition "<<pid<<" to node "<<world_rank<<std::endl;
	    	}
            single_start = single_end;
          } while(single_end < end);
	      // std::cout<<"4, # labels in partition, "<<pid<<" is "<<world_rank<<std::endl;
	}

	unsigned int mapPart2Node () {
	    num_parts = (1 + sqrt(1 + 8*world_size))/2;//solve the quadratic nC2 = q
	    query2node.resize(num_parts);
	    node2part.resize(world_size);
	    int nodeId = 0;
		//std::cout<<" num_parts: "<<num_parts<<std::endl;
	    for (size_t i=0; i<num_parts; i++)
	    {
	        query2node[i].resize(num_parts);
		}
	    for (size_t i=0; i<num_parts; i++)
		{
	        for (size_t j=i+1; j<num_parts; j++)
	        {
	            query2node[i][j] = nodeId; //assign nodes to overlapping partitions. 
	            query2node[j][i] = nodeId; //to retrieve -> nodeId for (u,v) = mapTable[min(u,v)][max(u,v)-min(u,v)]
	            node2part[nodeId].first = i;
	            node2part[nodeId].second = j;
	            nodeId++;
	        }
	        query2node[i][i] = query2node[i][(i+1)%num_parts];
	    }
	    assert(nodeId <= world_size);
	
	    //only nC2 nodes will be used. Some nodes will go unused
	    while(nodeId < world_size)
	    {
	        node2part[nodeId].first = num_parts + 1;
	        node2part[nodeId++].second = num_parts + 1;
	    }
	    return num_parts;
	}
	
	// distribute global labels of all partitions. Output -> partition pairs on each machine//
	unsigned int distAllPartitions ()
	{
		mapPart2Node();
//        if (world_rank==0) std::cout << "mapped partitions to nodes" << std::endl;
	    part_size = (n-1)/num_parts + 1;
	    for (unsigned int i=0; i<num_parts; i++)
	    { 
	        distPartition(i);
//            if (world_rank==0) std::cout << "distributed partition " << i << std::endl;
	    }
	    final_labeling.sort(omp_get_max_threads());
//      	std::cout<<"QDOL # labels in node, "<<world_rank<<", is, "<<final_labeling.get_total()<<std::endl;
//      	std::cout<<"QDOL # cap in node, "<<world_rank<<", is, "<<final_labeling.get_cap()<<std::endl;
	}
	
	//sort query list on basis of allocated node
	std::vector<Vertex> reorderQueryList(std::vector<Vertex> &queries, std::vector<int> &cnts, std::vector<int> &wr_offsets, std::vector<unsigned> &reorderMap)
	{
	    std::vector<std::vector<int>> cnts_per_node (NUM_THREAD, std::vector<int>(world_size, 0)); //every thread counts queries per remote node
	    std::vector<std::vector<int>> offsets (NUM_THREAD+1, std::vector<int>(world_size, 0)); //writing offsets per thread per remote node, in the final rearranged array
	    
	
	    std::vector<Vertex> temp_queries (queries.size()); //temporary vector to store rearranged queries
	    wr_offsets.resize(world_size);
	    unsigned int num_queries = queries.size()/2;
	    unsigned int num_queries_per_thread = (num_queries-1)/(NUM_THREAD) + 1;
		// if (world_rank== 0) std::cout<<"start count"<<std::endl;
	    reorderMap.resize(num_queries);
	
	    #pragma omp parallel num_threads(NUM_THREAD)
	    { 
	        #pragma omp for 
	        for (size_t i=0; i<NUM_THREAD; i++)
	        {
	            size_t start = i*num_queries_per_thread;
	            size_t end = std::min((i+1)*num_queries_per_thread, (size_t)num_queries);
	            for (size_t j=start; j<end; j++)
	            {
	                Vertex u = queries[(j<<1)];
	                Vertex v = queries[(j<<1) + 1];
					unsigned part_v = vertex2part(v);
					unsigned part_u = vertex2part(u);
	                cnts_per_node[i][query2node[part_u][part_v]]+=2; 
	            }
	            
	        }
	
	        #pragma omp barrier
	
	        //compute offsets for each thread within a block allocated to one node 
	        #pragma omp for 
	        for (size_t j=0; j<world_size; j++)
	        {
	            offsets[0][j] = 0;
	            for (size_t i=0; i<NUM_THREAD; i++)
	            {
	                offsets[i+1][j] = offsets[i][j] + cnts_per_node[i][j];
	                cnts_per_node[i][j] = 0; //reset counts to be used as wr pointer later;
	            }
	        }
	
	        #pragma omp barrier
			
	        //compute offsets per node
	        #pragma omp single
	        {
	            wr_offsets[0] = 0;
	            for (size_t i=1; i<world_size; i++)
	                wr_offsets[i] = wr_offsets[i-1] + offsets[NUM_THREAD][i-1];
	        }
	
	        #pragma omp barrier
			// if(world_rank==0) std::cout<<"node offset success"<<std::endl;
	
	        //rearrange queries
	        #pragma omp for
	        for (size_t i=0; i<NUM_THREAD; i++)
	        {
	            size_t start = i*num_queries_per_thread;
	            size_t end = std::min((i+1)*num_queries_per_thread,(size_t) num_queries);
	            for (size_t j=start; j<end; j++)
	            {
	                Vertex u = queries[j<<1];
	                Vertex v = queries[(j<<1) + 1];
					unsigned part_v = vertex2part(v);
					unsigned part_u = vertex2part(u);
	                int node = query2node[part_u][part_v];
	                int net_offset = wr_offsets[node] + offsets[i][node];
	                int wr_addr = net_offset + cnts_per_node[i][node];
	                temp_queries[wr_addr] = u;
	                temp_queries[wr_addr+1] = v;
	                cnts_per_node[i][node]+=2;
					reorderMap[wr_addr>>1] = j;
	            }
	        }
	    }
	    //queries.swap(temp_queries); 
	    cnts.swap(offsets[NUM_THREAD]);
		return temp_queries;
	}
	
	void reorderRes(std::vector<Distance> &dist, std::vector<unsigned> &reorderMap) {
	    std::vector<Distance> new_dist(dist.size());
	 	#pragma omp parallel for
	    for (size_t i=0; i<dist.size(); i++)  {
	 		unsigned loc = reorderMap[i];
	 		new_dist[loc] = dist[i];
	 	}
		dist.swap(new_dist);
	}
				
		
	
	void query(std::vector<Distance> &dist, std::vector<Vertex> &queries)
	{
	    int num_local_queries;
	    std::vector<Vertex> local_queries;
	    std::vector<Vertex> reordered_queries;
	    std::vector<Distance> temp_dist;
	    std::vector<int> cnts(world_size); //node-wise counts for scatter
	    std::vector<int> displs(world_size); //offsets
	    std::vector<unsigned> reorderMap; //offsets
	    dist.resize(queries.size()>>1);
	    //sort queries on target nodes
		if (world_rank==0) {	
			//std::cout<<"start reorder queries"<<std::endl;
		    reordered_queries = reorderQueryList(queries, cnts, displs, reorderMap);
//			std::cout<<"reorder queries succeed"<<std::endl;
			for (int i = 0; i<world_size; i++) {
//				std::cout<<"number of queries distributed to node "<<i<<" is "<<cnts[i]<<std::endl;
				//std::cout<<"offset distributed to node "<<i<<" is "<<displs[i]<<std::endl;
				//std::cout<<"example of "<<i<<" is "<<queries[displs[i]]<<" "<<queries[displs[i]+1]<<std::endl;
			}
		}
		double start = omp_get_wtime();	
		// if(world_rank==0)		std::cout<<"reorder succeed"<<std::endl;
		    //send the queries to target nodes
		MPI_Scatter(&cnts[0], 1, MPI_INT, &num_local_queries, 1, MPI_INT, 0, MPI_COMM_WORLD);
	//	std::cout<<world_rank<<" local query nums "<<num_local_queries<<std::endl;
		local_queries.resize(num_local_queries);
		MPI_Scatterv(&reordered_queries[0], &cnts[0], &displs[0], MPI_UNSIGNED, &local_queries[0], num_local_queries, MPI_UNSIGNED, 0, MPI_COMM_WORLD); 

		double scatterEnd = omp_get_wtime();
//  		if(world_rank==0)	std::cout<<"send queries succeed in time "<<scatterEnd-start<<std::endl;
		
	    //do label query
	    batchLocalQuery(temp_dist, local_queries, final_labeling, NUM_THREAD);

		double localQueryEnd = omp_get_wtime();
//  		if(world_rank==0)	std::cout<<"local queries succeed in time "<<localQueryEnd-scatterEnd<<std::endl;
  	//	std::cout<<world_rank<<" local queries succeed"<<std::endl;
//		if (world_rank != 3) std::cout<<"example of local query of node "<<world_rank<<" is Query("<<local_queries[0]<<", "<<local_queries[1]<<") = "<<temp_dist[0]<<std::endl;
	
	    //update volume indicators. Each query is 2 integers but query result is 1 integer
	    if (world_rank == 0) {
			for (int i=0; i<world_size; i++)
	 	  	{
	 	       cnts[i] = cnts[i]>>1;
	 	       displs[i] = displs[i]>>1;
	 	   }
		}
	    //gather results from remote nodes
	    MPI_Gatherv(&temp_dist[0], temp_dist.size(), MPI_INT, &dist[0], &cnts[0], &displs[0], MPI_INT, 0, MPI_COMM_WORLD); 
		double end = omp_get_wtime();
//		if (world_rank == 0) std::cout<<"part_pair "<<end-start<<" ";
	   	if (world_rank == 0) reorderRes(dist, reorderMap); 
	//	std::cout<<world_rank<<" gather succeed"<<std::endl;
	}


    int singleQuery(Distance &dist, std::vector<Vertex> &queries)
    {
        MPI_Status stat;
        int num_queries = 2;
        if (world_rank==0) {
            if (queries[0]>n){
                for (int i=1; i<world_size; i++)
                    MPI_Send(&queries[0], num_queries, MPI_INT, i, 0, MPI_COMM_WORLD);
            }
            else{
                unsigned part_u = vertex2part(queries[0]);
                unsigned part_v = vertex2part(queries[1]);
                int tgt_node = query2node[part_u][part_v];
				if (tgt_node == 0)
					singleLocalQuery(dist, queries, final_labeling);
				else
				{
                	MPI_Send(&queries[0], num_queries, MPI_INT, tgt_node, 0, MPI_COMM_WORLD);
                	MPI_Recv(&dist, num_queries, MPI_INT, tgt_node, 0, MPI_COMM_WORLD, &stat);
				}
            }
        }
        else {
            queries.resize(num_queries);
            MPI_Recv(&queries[0], num_queries, MPI_INT, 0, 0, MPI_COMM_WORLD, &stat); 
            if (queries[0] > n)
                return -1;
            singleLocalQuery(dist, queries, final_labeling);  
            MPI_Send(&dist, 1, MPI_INT, 0, 0, MPI_COMM_WORLD); 
        }
        return 0;
    }


	bool verify(std::vector<Vertex> &queries) {
		int q = queries.size() / 2;
		bool res = true;
		std::vector<Distance> mode1_res (q);
		std::vector<Distance> mode2_res (q);
		std::vector<Distance> mode3_res (q);
		double m1_start, m1_end, m1_time;
		double m2_start, m2_end, m2_time;
		double m3_start, m3_end, m3_time;

    	m2_start = omp_get_wtime();
		query(mode2_res, queries);
    	m2_end	 = omp_get_wtime();
		m3_start = omp_get_wtime();
		batchDistQuery(mode3_res, queries, curr_labeling, world_rank, world_size, NUM_THREAD);
    	m3_end	 = omp_get_wtime();
	///////////////mode 1////////////////
		std::vector<int> label_list;
		std::vector<int> recv_buffer;
		Labeling all_labels(n);
		parallelLoad(label_list, curr_labeling, 0, n, NUM_THREAD);
		gatherAllLabelsQuery(label_list, recv_buffer, world_size, world_rank);
		loadFromRecvBuffer(recv_buffer, all_labels);
		all_labels.sort(NUM_THREAD);
		//std::cout<<"all label size "<<all_labels.get_avg()<<std::endl;
		m1_start = omp_get_wtime();
		if(world_rank==0) batchLocalQuery(mode1_res, queries, all_labels, NUM_THREAD);
		m1_end	 = omp_get_wtime();
	//////////// mode 1 done ///////////
		if (world_rank==0)	{
			for (int i = 0; i <q; i++) {
				if (mode2_res[i]!=mode3_res[i] || mode1_res[i]!= mode2_res[i]) {
					std::cout<<"Diff happenes in  checking "<<queries[i*2] <<" to "<<queries[i*2+1] <<" mode 1: "<<mode1_res[i]<<" mode2: "<<mode2_res[i]<<" mode3: "<<mode3_res[i]<<std::endl;
				
					res = false;
				}
			}
			std::cout<<"mode 1: "<<m1_end-m1_start<<" mode 2: "<<m2_end-m2_start<<" mode 3: "<<m3_end-m3_start<<std::endl;
		}
		return (world_rank==0) ? res : true; 
	}	
};
}
