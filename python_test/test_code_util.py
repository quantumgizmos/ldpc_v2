from ldpc.codes import hamming_code, rep_code, ring_code, random_binary_code
import scipy.sparse
import numpy as np
from ldpc import mod2
from ldpc import code_util

def test_construct_generator_matrix():

    for i in range(3,10):

        H = hamming_code(i)
        G = code_util.construct_generator_matrix(H)
        assert not ((H@G.T).data %2).any()

def test_code_distance():

    for _ in range(10):
        H = random_binary_code(20,30,4, variance=0)
        d,s, cw = code_util.estimate_min_distance(H,0.025)
        assert not np.any( (H@cw.T).data%2 )

def test_code_distance_hamming():

    for i in range(3,12):

        H = hamming_code(i)
        d,s, cw = code_util.estimate_min_distance(H,0.025)
        assert not np.any( (H@cw.T).data%2 )
        assert d >= 3

def test_code_distance_ring():

    for i in range(3,25):

        H = ring_code(i)
        d,s, cw = code_util.estimate_min_distance(H,0.025)
        assert not np.any( (H@cw.T).data%2 )
        assert d == i

def test_compute_code_dimension():

    for i in range(3,12):

        H = hamming_code(i)
        assert code_util.compute_code_dimension(H) == H.shape[1] - H.shape[0]

        H = rep_code(i)
        assert code_util.compute_code_dimension(H) == 1

        H = ring_code(i)
        assert code_util.compute_code_dimension(H) == 1

def test_compute_code_parameters():

    for i in range(3,10):
        
        H = hamming_code(i)
        n, k, d = code_util.compute_code_parameters(H)
        assert n == H.shape[1]
        assert k == H.shape[1] - H.shape[0]
        assert d >= 3