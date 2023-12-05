#ifndef UF2_H
#define UF2_H

#include <iostream>
#include <vector>
#include <memory>
#include <iterator>
#include <limits>
#include <set>
#include <map>
#include "sparse_matrix_util.hpp"
#include "bp.hpp"
#include "osd.hpp"
#include <robin_map.h>
#include <robin_set.h>
#include <numeric>

#include "gf2sparse_linalg.hpp"
#include "bp.hpp"
#include "gf2dense.hpp"

namespace ldpc::uf {

    const std::vector<double> NULL_DOUBLE_VECTOR = {};

    std::vector<int> sort_indices(std::vector<double> &B) {
        std::vector<int> indices(B.size());
        std::iota(indices.begin(), indices.end(), 0);
        std::sort(indices.begin(), indices.end(), [&](int i, int j) { return B[i] < B[j]; });
        return indices;
    }

    struct Cluster {
        ldpc::bp::BpSparse &pcm;
        int cluster_id;
        bool active; // if merge one becomes deactivated
        bool valid; //
        tsl::robin_set<int> bit_nodes;
        tsl::robin_set<int> check_nodes;
        tsl::robin_set<int> boundary_check_nodes;
        std::vector<int> candidate_bit_nodes;
        tsl::robin_set<int> enclosed_syndromes;
        tsl::robin_map<int, int> spanning_tree_check_roots;
        tsl::robin_set<int> spanning_tree_bits;
        tsl::robin_set<int> spanning_tree_leaf_nodes;
        Cluster **global_check_membership; // store which cluster a check belongs to
        Cluster **global_bit_membership; // store which cluster a bit belongs to
        tsl::robin_set<Cluster *> merge_list;
        std::vector<int> cluster_decoding;
        std::vector<int> matrix_to_cluster_bit_map;
        tsl::robin_map<int, int> cluster_to_matrix_bit_map;
        std::vector<int> matrix_to_cluster_check_map;
        tsl::robin_map<int, int> cluster_to_matrix_check_map;
        gf2dense::CscMatrix cluster_pcm; // parity check matrix corresponding to the cluster
        std::vector<std::size_t> cluster_syndr_idx_to_pcm_idx;
        std::vector<std::size_t> cluster_check_to_pcm_check;
        std::vector<std::size_t> cluster_bit_to_pcm_bit;
        gf2dense::PluDecomposition *pluDecomposition;
        int eliminated_col_index = 0;
        int eliminated_row_index = 0;

        Cluster() = default;

        Cluster(ldpc::bp::BpSparse &parity_check_matrix,
                int syndrome_index,
                Cluster **ccm,
                Cluster **bcm) :
                pcm(parity_check_matrix) {
            this->active = true;
            this->valid = false;
            this->cluster_id = syndrome_index;
            this->boundary_check_nodes.insert(syndrome_index);
            this->check_nodes.insert(syndrome_index);
            this->enclosed_syndromes.insert(syndrome_index);
            this->global_check_membership = ccm;
            this->global_bit_membership = bcm;
            this->global_check_membership[syndrome_index] = this;
            // track cluster <> global syndrome indices mapping
            // cluster_syndr_idx_to_pcm_idx[i] = j means cluster row index i == global pcm index j
            this->cluster_syndr_idx_to_pcm_idx.resize(pcm.m);
            this->cluster_syndr_idx_to_pcm_idx[0] = syndrome_index;
        }

        ~Cluster() {
            this->bit_nodes.clear();
            this->check_nodes.clear();
            this->boundary_check_nodes.clear();
            this->candidate_bit_nodes.clear();
            this->enclosed_syndromes.clear();
            this->merge_list.clear();
            this->cluster_pcm.clear();
            this->pluDecomposition = nullptr;
            this->cluster_syndr_idx_to_pcm_idx.clear();
            this->eliminated_col_index = 0;
            this->eliminated_row_index = 0;
        }

        [[nodiscard]] int parity() const {
            return static_cast<int>(this->enclosed_syndromes.size() % 2);
        }

        int grow_cluster(const std::vector<double> &bit_weights = NULL_DOUBLE_VECTOR,
                         const int bits_per_step = 0) {
            if (!this->active) {
                return 0;
            }
            // compute a list of bit nodes to grow the cluster to
            this->compute_growth_candidate_bit_nodes();
            this->merge_list.clear();
            if (bit_weights == NULL_DOUBLE_VECTOR) {
                for (int bit_index: this->candidate_bit_nodes) {
                    this->add_bit_node_to_cluster(bit_index);
                }
            } else {
                std::vector<double> cluster_bit_weights;
                for (int bit: this->candidate_bit_nodes) {
                    cluster_bit_weights.push_back(bit_weights[bit]);
                }
                auto sorted_indices = sort_indices(cluster_bit_weights);
                int count = 0;
                for (int i: sorted_indices) {
                    if (count == bits_per_step) {
                        break;
                    }
                    int bit_index = this->candidate_bit_nodes[i];
                    this->add_bit_node_to_cluster(bit_index);
                    count++;
                }
            }
            this->merge_with_intersecting_clusters();
            return 1;
        }

        void merge_with_intersecting_clusters() {
            Cluster *larger = this;
            // merge with overlapping clusters while keeping the larger one always and deactivating the smaller ones
            for (auto cl: merge_list) {
                larger = merge_clusters(larger, cl);
            }
            // finally, we apply the on-the-fly elimination to the remaining cluster
            larger->apply_on_the_fly_elimination();
        }

        /**
         * Compute a list of candidate bit nodes to add to cluster as neighbours of boundary check nodes.
         * In case there are no new candidate bit nodes for a boundary check node, the check node is removed from the
         * boundary check node list.
         */
        void compute_growth_candidate_bit_nodes() {
            std::vector<int> boundary_checks_to_erase;
            this->candidate_bit_nodes.clear();
            // we check for new candidate bit nodes as neighbours of boundary check nodes
            for (auto check_index: boundary_check_nodes) {
                bool erase = true;
                for (auto &e: this->pcm.iterate_row(check_index)) {
                    // if bit is not in this cluster, add it to the candidate list.
                    if (this->global_bit_membership[e.col_index] != this) {
                        candidate_bit_nodes.push_back(e.col_index);
                        erase = false;
                    }
                }
                // erase from boundary check nodes if no neighbouring bits are added to candidate list.
                if (erase) {
                    boundary_checks_to_erase.push_back(check_index);
                }
            }
            for (auto check_index: boundary_checks_to_erase) {
                this->boundary_check_nodes.erase(check_index);
            }
        }

        /**
         * Adds a bit node to the cluster and updates all lists accordingly.
         * @param bit_index
         * @return true if the bit was added to the cluster, false otherwise.
         */
        bool add_bit_node_to_cluster(const int bit_index) {
            auto bit_membership = this->global_bit_membership[bit_index];
            //if the bit is already in the cluster return.
            if (bit_membership == this) {
                return false;
            } else if (bit_membership == nullptr) {
                //if the bit has not yet been assigned to a cluster we add it.
                this->add_bit(bit_index);
            } else {
                //if the bit already exists in a cluster, we mark down that this cluster should be
                //merged with the exisiting cluster.
                this->merge_list.insert(bit_membership);
                this->global_bit_membership[bit_index] = this;
            }
            // now we add the incident check nodes to the cluster.
            for (auto &e: this->pcm.iterate_column(bit_index)) {
                int check_index = e.row_index;
                auto check_membership = this->global_check_membership[check_index];
                if (check_membership == this) {
                    // if check is already in the cluster, go to next
                    continue;
                } else if (check_membership == nullptr) {
                    // check is in no cluster
                    this->add_check(check_index, true);
                } else {
                    // check is in another cluster
                    this->add_check(check_index, true);
                    this->merge_list.insert(check_membership);
                }
            }
            return true;
        }

        /**
         * Merge this cluster with another cluster and apply on the fly elimination
         * Keeps the larger cluster and merges the smaller cluster into it.
         * That is, the (reduced) parity check matrix of the larger cluster is kept.
         * @param cl2
         */
        static Cluster *merge_clusters(Cluster *cl1, Cluster *cl2) {
            Cluster *smaller;
            Cluster *larger;
            if (cl1->bit_nodes.size() < cl2->bit_nodes.size()) {
                smaller = cl1;
                larger = cl2;
            } else {
                smaller = cl2;
                larger = cl1;
            }
            // otherwise we merge cl2 into this cluster
            for (auto bit_index: smaller->bit_nodes) {
                larger->add_bit(bit_index);
            }
            for (auto check_index: smaller->check_nodes) {
                larger->add_check(check_index);
            }
            for (auto check_index: smaller->boundary_check_nodes) {
                larger->boundary_check_nodes.insert(check_index);
            }
            for (auto j: smaller->enclosed_syndromes) {
                larger->enclosed_syndromes.insert(j);
            }
            smaller->active = false; // smaller, absorbed cluster is deactivated
            return larger;
        }


        /**
         * Adds single check to cluster and updates all lists accordingly.
         * @param check_index
         * @param insert_boundary
         */
        void add_check(const int check_index, const bool insert_boundary = false) {
            if (insert_boundary) {
                this->boundary_check_nodes.insert(check_index);
            }
            if(this->eliminated_row_index == 0){
                this->eliminated_row_index = this->check_nodes.size();
            }
            this->check_nodes.insert(check_index);
            this->global_check_membership[check_index] = this;
            // todo I don't think we'll need to update the cluster_pcm here, since new rows are added with add_bit
            // we just need to make sure that the rows are 'complete' when applying the elimination procedure
        }

        /**
         * Adds single bit to cluster and updates all lists accordingly.
         * @param bit_index
         */
        void add_bit(const int bit_index) {
            // store column index offset for on the fly elimniation
            if (this->eliminated_col_index == 0){
                this->eliminated_col_index = this->bit_nodes.size();
            }
            this->bit_nodes.insert(bit_index);
            this->global_bit_membership[bit_index] = this;
            // also add to cluster pcm
            this->cluster_pcm.push_back(pcm.get_column_csc(bit_index));
            this->cluster_bit_to_pcm_bit.push_back(bit_index);
        }

        int find_spanning_tree_parent(const int check_index) {
            int parent = this->spanning_tree_check_roots[check_index];
            if (parent != check_index) {
                return find_spanning_tree_parent(parent);
            } else return parent;
        }

        void find_spanning_tree() {
            this->spanning_tree_bits.clear();
            this->spanning_tree_check_roots.clear();
            this->spanning_tree_leaf_nodes.clear();

            for (int bit_index: this->bit_nodes) {
                this->spanning_tree_bits.insert(bit_index);
            }

            for (int check_index: this->check_nodes) {
                this->spanning_tree_check_roots[check_index] = check_index;
            }

            int check_neighbours[2];
            for (int bit_index: this->bit_nodes) {
                check_neighbours[0] = this->pcm.column_heads[bit_index]->up->row_index;
                check_neighbours[1] = this->pcm.column_heads[bit_index]->down->row_index;

                int root0 = this->find_spanning_tree_parent(check_neighbours[0]);
                int root1 = this->find_spanning_tree_parent(check_neighbours[1]);

                if (root0 != root1) {
                    this->spanning_tree_check_roots[root1] = root0;
                } else {
                    this->spanning_tree_bits.erase(bit_index);
                }
            }

            for (int check_index: this->check_nodes) {
                int spanning_tree_connectivity = 0;
                for (auto &e: this->pcm.iterate_row(check_index)) {
                    if (this->spanning_tree_bits.contains(e.col_index)) spanning_tree_connectivity += 1;
                }
                if (spanning_tree_connectivity == 1) this->spanning_tree_leaf_nodes.insert(check_index);
            }
        }

        std::vector<int> peel_decode(const std::vector<uint8_t> &syndrome) {
            std::vector<int> erasure;
            tsl::robin_set<int> synds;
            for (auto check_index: check_nodes) {
                if (syndrome[check_index] == 1) synds.insert(check_index);
            }

            this->find_spanning_tree();
            while (synds.size() > 0) {

                int leaf_node_index = *(this->spanning_tree_leaf_nodes.begin());
                int bit_index = -1;
                int check2 = -1;

                for (auto &e: this->pcm.iterate_row(leaf_node_index)) {
                    bit_index = e.col_index;
                    if (this->spanning_tree_bits.contains(bit_index)) break;
                }


                for (auto &e: this->pcm.iterate_column(bit_index)) {
                    if (e.row_index != leaf_node_index) check2 = e.row_index;
                }


                if (synds.contains(leaf_node_index)) {
                    this->spanning_tree_leaf_nodes.erase(leaf_node_index);
                    // this->spanning_tree_leaf_nodes.insert(check2);
                    erasure.push_back(bit_index);
                    this->spanning_tree_bits.erase(bit_index);
                    if (synds.contains(check2)) synds.erase(check2);
                    else synds.insert(check2);
                    synds.erase(leaf_node_index);
                } else {
                    this->spanning_tree_leaf_nodes.erase(leaf_node_index);
                    // this->spanning_tree_leaf_nodes.insert(check2);
                    this->spanning_tree_bits.erase(bit_index);
                }

                //check whether new check node is a leaf
                int spanning_tree_connectivity = 0;
                for (auto &e: this->pcm.iterate_row(check2)) {
                    if (this->spanning_tree_bits.contains(e.col_index)) spanning_tree_connectivity += 1;
                }
                if (spanning_tree_connectivity == 1) this->spanning_tree_leaf_nodes.insert(check2);

            }

            return erasure;
        }

        ldpc::bp::BpSparse convert_to_matrix(const std::vector<double> &bit_weights = NULL_DOUBLE_VECTOR) {
            this->matrix_to_cluster_bit_map.clear();
            this->matrix_to_cluster_check_map.clear();
            this->cluster_to_matrix_bit_map.clear();
            this->cluster_to_matrix_check_map.clear();

            if (bit_weights != NULL_DOUBLE_VECTOR) {
                std::vector<double> cluster_bit_weights;
                std::vector<int> bit_nodes_temp;
                for (int bit: this->bit_nodes) {
                    cluster_bit_weights.push_back(bit_weights[bit]);
                    bit_nodes_temp.push_back(bit);
                }
                auto sorted_indices = sort_indices(cluster_bit_weights);
                int count = 0;
                for (int i: sorted_indices) {
                    int bit_index = bit_nodes_temp[i];
                    this->matrix_to_cluster_bit_map.push_back(bit_index);
                    this->cluster_to_matrix_bit_map[bit_index] = count;
                    count++;
                }
            } else {
                int count = 0;
                for (int bit_index: this->bit_nodes) {
                    this->matrix_to_cluster_bit_map.push_back(bit_index);
                    this->cluster_to_matrix_bit_map[bit_index] = count;
                    count++;
                }
            }
            int count = 0;

            for (int check_index: this->check_nodes) {
                this->matrix_to_cluster_check_map.push_back(check_index);
                this->cluster_to_matrix_check_map[check_index] = count;
                count++;
            }

            auto cluster_pcm = ldpc::bp::BpSparse(this->check_nodes.size(), this->bit_nodes.size());

            for (int check_index: this->check_nodes) {
                for (auto &e: this->pcm.iterate_row(check_index)) {
                    int bit_index = e.col_index;
                    if (this->bit_nodes.contains(bit_index)) {
                        int matrix_bit_index = cluster_to_matrix_bit_map[bit_index];
                        int matrix_check_index = cluster_to_matrix_check_map[check_index];
                        cluster_pcm.insert_entry(matrix_check_index, matrix_bit_index);
                    }
                }
            }
            return cluster_pcm;
        }


        std::vector<int> invert_decode(const std::vector<uint8_t> &syndrome,
                                       const std::vector<double> &bit_weights) {

            auto cluster_pcm = this->convert_to_matrix(bit_weights);
            std::vector<uint8_t> cluster_syndrome;
            for (int check_index: check_nodes) {
                cluster_syndrome.push_back(syndrome[check_index]);
            }
            auto rr = ldpc::gf2sparse_linalg::RowReduce(cluster_pcm);
            auto cluster_solution = rr.fast_solve(cluster_syndrome);
            auto candidate_cluster_syndrome = cluster_pcm.mulvec(cluster_solution);
            bool equal = true;

            for (int i = 0; i < cluster_syndrome.size(); i++) {
                if (cluster_syndrome[i] != candidate_cluster_syndrome[i]) {
                    equal = false;
                    break;
                }
            }
            this->cluster_decoding.clear();
            this->valid = equal;
            for (int i = 0; i < cluster_solution.size(); i++) {
                if (cluster_solution[i] == 1) {
                    this->cluster_decoding.push_back(this->matrix_to_cluster_bit_map[i]);
                }
            }
            return this->cluster_decoding;
        }

        void apply_on_the_fly_elimination() {
            // todo apply row operations here on new bits
            // todo apply on syndrome as well. For that we can add syndrome as column to cluster matrix I suppose
            // todo eliminate newly added rows using partial_rref function

            // check if syndrome is in image, if so mark the cluster as valid and store the decoding estimate
            if (this->pluDecomposition == nullptr) {
                // no existing decomposition yet so we create one
                this->pluDecomposition = new ldpc::gf2dense::PluDecomposition(this->cluster_pcm.size(),
                                                                              this->cluster_pcm.at(0).size(),
                                                                              this->cluster_pcm);
            }
            auto elimination_start_column = this->eliminated_col_index;
            auto elimination_start_row = this->eliminated_row_index;
            // apply stored operation to newly added columns, i.e., those starting at eliminated_col_index
            this->pluDecomposition->apply_stored_operations(elimination_start_column);
            // apply elimination for all newly added rows, i.e., those starting at elimination_start_row
            this->pluDecomposition->partial_rref(elimination_start_row);

            // todo here, we check if the syndrome is in the image.
            // if so, we are done, the cluster is valid and the syndrome can be stored as cluster syndrome
            // otherwise, we continue

            // reset offsets for next growth step
            this->eliminated_col_index = 0;
            this->eliminated_row_index = 0;
        }

        std::vector<int> invert_decode2(const std::vector<uint8_t> &syndrome, std::vector<double> &bit_weights) {
            auto cluster_pcm = this->convert_to_matrix(bit_weights);
            std::cout << "After cluter pcm gen" << std::endl;
            std::vector<uint8_t> cluster_syndrome;
            int synd_weight = 0;
            for (int check_index: check_nodes) {
                cluster_syndrome.push_back(syndrome[check_index]);
                synd_weight += syndrome[check_index];
            }
            this->cluster_decoding.clear();
            bool equal;
            if (synd_weight > 0) {
                auto rr = ldpc::gf2sparse_linalg::RowReduce(cluster_pcm);
                auto cluster_solution = rr.fast_solve(cluster_syndrome);
                auto candidate_cluster_syndrome = cluster_pcm.mulvec(cluster_solution);
                equal = true;
                for (int i = 0; i < cluster_syndrome.size(); i++) {
                    if (cluster_syndrome[i] != candidate_cluster_syndrome[i]) {
                        equal = false;
                        break;
                    }
                }
                for (int i = 0; i < cluster_solution.size(); i++) {
                    if (cluster_solution[i] == 1) {
                        this->cluster_decoding.push_back(this->matrix_to_cluster_bit_map[i]);
                    }
                }

            } else {
                equal = false;
            }
            this->valid = equal;
            return this->cluster_decoding;
        }

        void print();
    };


    Cluster *bit_cluster(ldpc::bp::BpSparse &parity_check_matrix, int bit_index, Cluster **ccm, Cluster **bcm) {

        Cluster *cl = new Cluster(parity_check_matrix, bit_index, ccm, bcm);


        cl->global_check_membership[bit_index] = NULL;
        cl->global_bit_membership[bit_index] = cl;
        cl->bit_nodes.insert(bit_index);
        cl->check_nodes.clear();
        cl->enclosed_syndromes.clear();
        cl->boundary_check_nodes.clear();


        for (auto &e: cl->pcm.iterate_column(bit_index)) {
            int check_index = e.row_index;
            cl->check_nodes.insert(check_index);
            cl->boundary_check_nodes.insert(check_index);
            cl->global_check_membership[check_index] = cl;
        }

        return cl;

    }


    class UfDecoder {

    private:
        bool weighted;
        ldpc::bp::BpSparse &pcm;

    public:
        std::vector<uint8_t> decoding;
        int bit_count;
        int check_count;

        UfDecoder(ldpc::bp::BpSparse &parity_check_matrix) : pcm(parity_check_matrix) {
            this->bit_count = pcm.n;
            this->check_count = pcm.m;
            this->decoding.resize(this->bit_count);
            this->weighted = false;
        }

        std::vector<uint8_t> &
        peel_decode(const std::vector<uint8_t> &syndrome, const std::vector<double> &bit_weights = NULL_DOUBLE_VECTOR,
                    int bits_per_step = 1) {

            fill(this->decoding.begin(), this->decoding.end(), 0);

            std::vector<Cluster *> clusters;
            std::vector<Cluster *> invalid_clusters;
            Cluster **global_bit_membership = new Cluster *[pcm.n]();
            Cluster **global_check_membership = new Cluster *[pcm.m]();

            for (int i = 0; i < this->pcm.m; i++) {
                if (syndrome[i] == 1) {
                    Cluster *cl = new Cluster(this->pcm, i, global_check_membership, global_bit_membership);
                    clusters.push_back(cl);
                    invalid_clusters.push_back(cl);
                }
            }

            while (invalid_clusters.size() > 0) {

                for (auto cl: invalid_clusters) {
                    if (cl->active) {
                        cl->grow_cluster(bit_weights, bits_per_step);
                    }
                }

                invalid_clusters.clear();
                for (auto cl: clusters) {
                    if (cl->active == true && cl->parity() == 1) {
                        invalid_clusters.push_back(cl);
                    }
                }

                std::sort(invalid_clusters.begin(), invalid_clusters.end(), [](const Cluster *lhs, const Cluster *rhs) {
                    return lhs->bit_nodes.size() < rhs->bit_nodes.size();
                });

            }

            for (auto cl: clusters) {
                if (cl->active) {
                    auto erasure = cl->peel_decode(syndrome);
                    for (int bit: erasure) {
                        this->decoding[bit] = 1;
                    }
                }
                delete cl;
            }

            delete[] global_bit_membership;
            delete[] global_check_membership;

            return this->decoding;

        }


        std::vector<uint8_t> &
        matrix_decode(const std::vector<uint8_t> &syndrome,
                      const std::vector<double> &bit_weights = NULL_DOUBLE_VECTOR,
                      const int bits_per_step = 1) {

            fill(this->decoding.begin(), this->decoding.end(), 0);

            std::vector<Cluster *> clusters;
            std::vector<Cluster *> invalid_clusters;
            auto **global_bit_membership = new Cluster *[pcm.n]();
            auto **global_check_membership = new Cluster *[pcm.m]();

            for (auto i = 0; i < this->pcm.m; i++) {
                if (syndrome[i] == 1) {
                    auto *cl = new Cluster(this->pcm, i, global_check_membership, global_bit_membership);
                    clusters.push_back(cl);
                    invalid_clusters.push_back(cl);
                }
            }

            while (!invalid_clusters.empty()) {
                for (auto cl: invalid_clusters) {
                    if (cl->active) {
                        cl->grow_cluster(bit_weights, bits_per_step);
                        auto cluster_decoding = cl->invert_decode(syndrome, bit_weights);
                    }
                }
                invalid_clusters.clear();
                for (auto cl: clusters) {
                    if (cl->active && !cl->valid) {
                        invalid_clusters.push_back(cl);
                    }
                }
                std::sort(invalid_clusters.begin(), invalid_clusters.end(),
                          [](const Cluster *lhs, const Cluster *rhs) {
                              return lhs->bit_nodes.size() < rhs->bit_nodes.size();
                          });
            }

            for (auto cl: clusters) {
                if (cl->active) {
                    for (auto bit: cl->cluster_decoding) {
                        this->decoding[bit] = 1;
                    }
                }
                delete cl;
            }
            delete[] global_bit_membership;
            delete[] global_check_membership;
            return this->decoding;
        }

        std::vector<uint8_t> &bit_cluster_decode(const std::vector<uint8_t> &syndrome, std::vector<double> &bit_weights,
                                                 int bits_per_step = 1, int cluster_count = 10) {

            fill(this->decoding.begin(), this->decoding.end(), 0);

            std::vector<Cluster *> clusters;
            std::vector<Cluster *> invalid_clusters;
            Cluster **global_bit_membership = new Cluster *[pcm.n]();
            Cluster **global_check_membership = new Cluster *[pcm.m]();

            std::vector<int> sparse_syndrome;


            for (int i = 0; i < this->pcm.m; i++) {
                if (syndrome[i] == 1) {
                    sparse_syndrome.push_back(i);
                }
            }


            std::vector<int> col_indices;
            for (int i = 0; i < this->pcm.n; i++) {
                col_indices.push_back(i);
            }

            ldpc::sort::soft_decision_col_sort(bit_weights, col_indices, this->pcm.n);

            int max_clusters = std::min(cluster_count, pcm.n);

            for (int i = 0; i < max_clusters; i++) {

                int bit_index = col_indices[i];

                Cluster *cl = new Cluster(this->pcm, bit_index, global_check_membership, global_bit_membership);
                cl->active = true;
                cl->valid = false;
                cl->cluster_id = bit_index;
                cl->boundary_check_nodes.clear();
                cl->check_nodes.clear();
                cl->enclosed_syndromes.clear();
                cl->global_check_membership[bit_index] = NULL;
                cl->global_bit_membership[bit_index] = cl;

                clusters.push_back(cl);
                invalid_clusters.push_back(cl);

            }

            // while(invalid_clusters.size()>0){

            //     for(auto cl: invalid_clusters){
            //         if(cl->active){
            //             std::cout<<cl<<std::endl;
            //             cl->print();

            //             cl->grow_cluster(bit_weights,bits_per_step);
            //             std::cout<<"HEllo64"<<std::endl;

            //             auto cluster_decoding = cl->invert_decode2(syndrome,bit_weights);
            //              std::cout<<"HEllo66"<<std::endl;
            //         }
            //     }



            //     invalid_clusters.clear();
            //     for(auto cl: clusters){
            //         if(cl->active == true && cl->valid == false){
            //             invalid_clusters.push_back(cl);
            //         }
            //     }



            //     sort(invalid_clusters.begin(), invalid_clusters.end(), [](const Cluster* lhs, const Cluster* rhs){return lhs->bit_nodes.size() < rhs->bit_nodes.size();});

            //     bool exit_while = true;
            //     for(auto synd_index: sparse_syndrome){
            //         auto cl = global_check_membership[synd_index];
            //         if(cl==NULL){
            //             exit_while = false;
            //         }
            //         else if (cl->valid == false){
            //             exit_while = false;
            //         }

            //     }



            //     if(exit_while) break;



            // }




            for (auto cl: clusters) {
                if (cl->active) {
                    for (int bit: cl->cluster_decoding) this->decoding[bit] = 1;
                }
                // delete cl;
            }


            std::cout << "HEllo7" << std::endl;


            // delete[] global_bit_membership;
            // delete[] global_check_membership;

            // std::cout<<"hello from end of C++ function"<<std::endl;




            std::cout << "HEllo3" << std::endl;
            ldpc::sparse_matrix_util::print_vector(this->decoding);


            return this->decoding;

        }

    };

    void Cluster::print() {
        std::cout << "........." << std::endl;
        std::cout << "Cluster ID: " << this->cluster_id << std::endl;
        std::cout << "Active: " << this->active << std::endl;
        std::cout << "Enclosed syndromes: ";
        for (auto i: this->enclosed_syndromes) std::cout << i << " ";
        std::cout << std::endl;
        std::cout << "Cluster bits: ";
        for (auto i: this->bit_nodes) std::cout << i << " ";
        std::cout << std::endl;
        std::cout << "Cluster checks: ";
        for (auto i: this->check_nodes) std::cout << i << " ";
        std::cout << std::endl;
        std::cout << "Candidate bits: ";
        for (auto i: this->candidate_bit_nodes) std::cout << i << " ";
        std::cout << std::endl;
        std::cout << "Boundary Checks: ";
        for (auto i: this->boundary_check_nodes) std::cout << i << " ";
        std::cout << std::endl;
        std::cout << "Spanning tree: ";
        for (auto i: this->spanning_tree_bits) std::cout << i << " ";
        std::cout << std::endl;
        std::cout << "........." << std::endl;
    }


}//end namespace uf

#endif