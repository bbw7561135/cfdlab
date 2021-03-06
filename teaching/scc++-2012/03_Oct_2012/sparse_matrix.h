#ifndef __SPARSE_MATRIX_H__
#define __SPARSE_MATRIX_H__

#include <iomanip>
#include <vector>

#include "Vector.h"

template <class T>
class SparseMatrix
{
   public:
      SparseMatrix (std::vector<unsigned int>& row_ptr, 
                    std::vector<unsigned int>& col_ind, 
                               std::vector<T>& val);
      ~SparseMatrix() {};
      unsigned int size () const 
      {
         return nrow;
      }
      void multiply(const Vector<T>& x, 
                          Vector<T>& y) const;
      T operator()(unsigned int i, 
                   unsigned int j) const;
      friend std::ostream& operator<< (std::ostream&          os, 
                                       const SparseMatrix<T>& A)
      {
         for(unsigned int i=0; i<A.size(); ++i)
         {
            for(unsigned int j=0; j<A.size(); ++j)
               os << std::setw(10) << A(i,j);
            os << std::endl;
         }
         return os;
      }
      
   private:
      unsigned int nrow;
      std::vector<unsigned int> row_ptr, col_ind;
      std::vector<T> val;
};

#endif
