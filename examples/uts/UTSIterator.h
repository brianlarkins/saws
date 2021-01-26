#ifndef __UTSIterator_h__
#define __UTSIterator_h__
 
#include "uts.h"

class UTSIterator {
  private:
    Node node;         // Tree node, contains the 20-byte SHA-1 hash
    int processed;     // Has the task associated with this iterator been processed?
    int current_child; // Index of next child to create

    static counter_t nNodes;
    static counter_t nLeaves;
    static counter_t maxDepth;

  public:
    UTSIterator(int type);   // Root Iterator
    UTSIterator();           // NULL Iterator
    UTSIterator(Node child); // Child Iterator
    
    bool        hasNext();
    UTSIterator next();
    void        next(UTSIterator *nextit);
    void        process();
    
    static void resetStats();
    static counter_t get_nNodes();
    static counter_t get_nLeaves();
    static counter_t get_maxDepth();
}; /* class UTSIterator */

#endif /*__UTSIterator_h__*/


