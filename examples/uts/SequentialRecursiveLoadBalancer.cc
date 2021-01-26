#include "UTSIterator.h" 

void ldbal_sequential(UTSIterator iter) {  
		iter.process();   

		while (iter.hasNext())    
			ldbal_sequential(iter.next());
} 
