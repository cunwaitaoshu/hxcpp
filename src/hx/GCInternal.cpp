#include <hxcpp.h>

#include <hx/GC.h>
#include <hx/Thread.h>

char **gMovedPtrs = 0;
int gByteMarkID = 0;
int gMarkID = 0;


#ifdef HX_INTERNAL_GC

#ifdef _MSC_VER
#include <windows.h>
#endif

#include <map>
#include <vector>
#include <set>



static bool sgAllocInit = 0;
static bool sgInternalEnable = false;
static void *sgObject_root = 0;

static int sgTimeToNextTableUpdate = 0;


MyMutex  *gThreadStateChangeLock=0;

class LocalAllocator;
enum LocalAllocState { lasNew, lasRunning, lasStopped, lasWaiting, lasTerminal };
static bool sMultiThreadMode = false;

TLSData<LocalAllocator> tlsLocalAlloc;

static void MarkLocalAlloc(LocalAllocator *inAlloc);
static void WaitForSafe(LocalAllocator *inAlloc);
static void ReleaseFromSafe(LocalAllocator *inAlloc);

namespace hx {
int gPauseForCollect = 0;
void ExitGCFreeZoneLocked();
}

//#define DEBUG_ALLOC_PTR ((char *)0xb68354)

template<typename T>
struct QuickVec
{
   QuickVec() : mPtr(0), mAlloc(0), mSize(0) { } 
   inline void push(T inT)
   {
      if (mSize+1>=mAlloc)
      {
         mAlloc = 10 + (mSize*3/2);
         mPtr = (T *)realloc(mPtr,sizeof(T)*mAlloc);
      }
      mPtr[mSize++]=inT;
   }
   inline T pop()
   {
      return mPtr[--mSize];
   }
   inline void qerase(int inPos)
   {
      --mSize;
      mPtr[inPos] = mPtr[mSize];
   }
   inline void erase(int inPos)
   {
      --mSize;
      if (mSize>inPos)
         memmove(mPtr+inPos, mPtr+inPos+1, (mSize-inPos)*sizeof(T));
   }

	inline void qerase_val(T inVal)
   {
      for(int i=0;i<mSize;i++)
         if (mPtr[i]==inVal)
         {
            --mSize;
            mPtr[i] = mPtr[mSize];
            return;
         }
   }

   inline bool empty() const { return !mSize; }
   inline void clear() { mSize = 0; }
   inline int next()
   {
      if (mSize+1>=mAlloc)
      {
         mAlloc = 10 + (mSize*3/2);
         mPtr = (T *)realloc(mPtr,sizeof(T)*mAlloc);
      }
      return mSize++;
   }
   inline int size() const { return mSize; }
   inline T &operator[](int inIndex) { return mPtr[inIndex]; }

   int mAlloc;
   int mSize;
   T *mPtr;
};


// --- InternalFinalizer -------------------------------

namespace hx
{

typedef QuickVec<InternalFinalizer *> FinalizerList;

FinalizerList *sgFinalizers = 0;

InternalFinalizer::InternalFinalizer(hx::Object *inObj)
{
   mUsed = false;
   mValid = true;
   mObject = inObj;
   mFinalizer = 0;
   sgFinalizers->push(this);
}

void InternalFinalizer::Detach()
{
   mValid = false;
}

void RunFinalizers()
{
   FinalizerList &list = *sgFinalizers;
   int idx = 0;
   while(idx<list.size())
   {
      InternalFinalizer *f = list[idx];
      if (!f->mValid)
         list.qerase(idx);
      else if (!f->mUsed)
      {
         if (f->mFinalizer)
            f->mFinalizer(f->mObject);
         list.qerase(idx);
         delete f;
      }
      else
      {
         f->mUsed = false;
         idx++;
      }
   }
}



void InternalEnableGC(bool inEnable)
{
   sgInternalEnable = inEnable;
}


void *InternalCreateConstBuffer(const void *inData,int inSize)
{
   int *result = (int *)malloc(inSize + sizeof(int));

   *result = 0xffffffff;
   memcpy(result+1,inData,inSize);

   return result+1;
}

} // end namespace hx

// ---  Internal GC - IMMIX Implementation ------------------------------



// Some inline implementations ...
// Use macros to allow for mark/move



/*
  IMMIX block size, and various masks for converting addresses

*/

#define IMMIX_BLOCK_BITS      15
#define IMMIX_LINE_BITS        7

#define IMMIX_BLOCK_SIZE        (1<<IMMIX_BLOCK_BITS)
#define IMMIX_BLOCK_OFFSET_MASK (IMMIX_BLOCK_SIZE-1)
#define IMMIX_BLOCK_BASE_MASK   (~(size_t)(IMMIX_BLOCK_OFFSET_MASK))
#define IMMIX_LINE_LEN          (1<<IMMIX_LINE_BITS)
#define IMMIX_LINES             (1<<(IMMIX_BLOCK_BITS-IMMIX_LINE_BITS))
#define IMMIX_HEADER_LINES      (IMMIX_LINES>>IMMIX_LINE_BITS)
#define IMMIX_USEFUL_LINES      (IMMIX_LINES - IMMIX_HEADER_LINES)
#define IMMIX_LINE_POS_MASK     ((size_t)(IMMIX_LINE_LEN-1))
#define IMMIX_START_OF_ROW_MASK  (~IMMIX_LINE_POS_MASK)



/*

 IMMIX Alloc Header - 32 bits

*/

#if HX_LITTLE_ENDIAN
#define ENDIAN_MARK_ID_BYTE        -1
#define ENDIAN_OBJ_NEXT_BYTE       2
#else
#define ENDIAN_MARK_ID_BYTE        -4
#define ENDIAN_OBJ_NEXT_BYTE       1
#endif

#define IMMIX_ALLOC_MARK_ID     0xff000000
#define IMMIX_ALLOC_OBJ_NEXT    0x00ff0000
#define IMMIX_ALLOC_IS_OBJECT   0x00008000
#define IMMIX_ALLOC_IS_CONST    0x00004000
#define IMMIX_ALLOC_SIZE_MASK   0x00003ffc
#define IMMIX_ALLOC_MEDIUM_OBJ  0x00000002
#define IMMIX_ALLOC_SMALL_OBJ   0x00000001



/*

 IMMIX Row Header - 8 bits

*/
#define IMMIX_ROW_CLEAR           0x80
#define IMMIX_ROW_LINK_MASK       0x7C
#define IMMIX_ROW_HAS_OBJ_LINK    0x02
#define IMMIX_ROW_MARKED          0x01
#define IMMIX_NOT_MARKED_MASK     (~IMMIX_ROW_MARKED)


// Bigger than this, and they go in the large object pool
#define IMMIX_LARGE_OBJ_SIZE 4000

#ifdef allocString
#undef allocString
#endif



enum AllocType { allocNone, allocString, allocObject, allocMarked };

union BlockData
{
   void Init() { mUsedRows = 0; }
   inline int GetFreeData() const { return (IMMIX_USEFUL_LINES - mUsedRows)<<IMMIX_LINE_BITS; }
   void ClearEmpty()
   {
      memset(this,0,IMMIX_HEADER_LINES * IMMIX_LINE_LEN);
      memset(mRow[IMMIX_HEADER_LINES],0,IMMIX_USEFUL_LINES * IMMIX_LINE_LEN);
      mUsedRows = 0;
   }
   void ClearRecycled()
   {
      for(int r=IMMIX_HEADER_LINES;r<IMMIX_LINES;r++)
      {
         unsigned char &flags = mRowFlags[r];
         if (!(flags & IMMIX_ROW_MARKED) /*&& !(flags & IMMIX_ROW_CLEAR) */)
         {
            //__int64 *row = (__int64 *)mRow[r];
            double *row = (double *)mRow[r];
            row[0] = 0;
            row[1] = 0;
            row[2] = 0;
            row[3] = 0;
            row[4] = 0;
            row[5] = 0;
            row[6] = 0;
            row[7] = 0;
            row[8] = 0;
            row[9] = 0;
            row[10] = 0;
            row[11] = 0;
            row[12] = 0;
            row[13] = 0;
            row[14] = 0;
            row[15] = 0;
            //flags = IMMIX_ROW_CLEAR;
            flags = 0;
         }
      }
   }
   void DirtyLines(int inFirst,int inN)
   {
      unsigned char *ptr = mRowFlags + inFirst;
      for(int i=0;i<inN;i++)
         ptr[i] &= ~(IMMIX_ROW_CLEAR);
   }
   bool IsEmpty() const { return mUsedRows == 0; }
   bool IsFull() const { return mUsedRows == IMMIX_USEFUL_LINES; }
   int GetRowsInUse() const { return mUsedRows; }
   inline bool IsRowUsed(int inRow) const { return mRowFlags[inRow] & IMMIX_ROW_MARKED; }

   void Verify()
   {
      for(int r=IMMIX_HEADER_LINES;r<IMMIX_LINES;r++)
      {
         unsigned char &row_flag = mRowFlags[r];
         if ( !(row_flag * IMMIX_ROW_MARKED) )
         {
            if (row_flag!=0)
            {
               printf("Block verification failed on row %d\n",r);
               *(int *)0=0;
            }
         }
      }
   }

   #define CHECK_TABLE_LIVE \
      if (*table && ((row[*table]) !=  gByteMarkID)) *table = 0;

   void Reclaim()
   {
      int free = 0;
      int max_free_in_a_row = 0;
      int free_in_a_row = 0;
      bool update_table = sgTimeToNextTableUpdate==0;
      for(int r=IMMIX_HEADER_LINES;r<IMMIX_LINES;r++)
      {
         unsigned char &row_flag = mRowFlags[r];
         if (row_flag & IMMIX_ROW_MARKED)
         {
            if (update_table)
            {
               // Must update from the object mark flag ...
               if (row_flag & IMMIX_ROW_HAS_OBJ_LINK)
               {
                  unsigned char *row = mRow[r];
                  unsigned char *last_link = &row_flag;
                  int pos = (row_flag & IMMIX_ROW_LINK_MASK);
                  while(1)
                  {
                     if (row[pos+3] == gByteMarkID)
                     {
                        *last_link = pos | IMMIX_ROW_HAS_OBJ_LINK;
                        last_link = row+pos+2;
                     }
                     if (row[pos+2] & IMMIX_ROW_HAS_OBJ_LINK)
                        pos = row[pos+2] & IMMIX_ROW_LINK_MASK;
                     else
                        break;
                  }
                  *last_link = 0;
                  row_flag |= IMMIX_ROW_MARKED;
               }
            }
            free_in_a_row = 0;
         }
         else
         {
            row_flag = 0;
            free_in_a_row++;
            if (free_in_a_row>max_free_in_a_row)
               max_free_in_a_row = free_in_a_row;
            free++;
         }
      }

      mUsedRows = IMMIX_USEFUL_LINES - free;
      mFreeInARow = max_free_in_a_row;

      //Verify();
   }


   AllocType GetAllocType(int inOffset,bool inReport = false)
   {
      inReport = false;
      int r = inOffset >> IMMIX_LINE_BITS;
      if (r < IMMIX_HEADER_LINES || r >= IMMIX_LINES)
      {
         if (inReport)
            printf("  bad row %d (off=%d)\n", r);
         return allocNone;
      }
      unsigned char time = mRow[0][inOffset+3];
      if ( ((time+1) & 0xff) != gByteMarkID )
      {
         // Object is either out-of-date, or already marked....
         return time==gByteMarkID ? allocMarked : allocNone;
      }

      int flags = mRowFlags[r];
      if (!(flags & (IMMIX_ROW_HAS_OBJ_LINK)))
      {
         if (inReport)
            printf("  row has no new objs :[%d] = %d\n", r, flags );
         return allocNone;
      }


      int sought = (inOffset & IMMIX_LINE_POS_MASK);
      unsigned char *row = mRow[r];
      int pos = (flags & IMMIX_ROW_LINK_MASK);

      while( pos!=sought && (row[pos+ENDIAN_OBJ_NEXT_BYTE] & IMMIX_ROW_HAS_OBJ_LINK) )
         pos = row[pos+ENDIAN_OBJ_NEXT_BYTE] & IMMIX_ROW_LINK_MASK;

      if (pos==sought)
         return (*(unsigned int *)(mRow[0] + inOffset) & IMMIX_ALLOC_IS_OBJECT) ?
            allocObject: allocString;

      if (inReport)
      {
         printf("  not found in table (r=%d,sought =%d): ", row, sought);
         int pos = (flags & IMMIX_ROW_LINK_MASK);
         printf(" %d ", pos );
         while( pos!=sought && (row[pos+ENDIAN_OBJ_NEXT_BYTE] & IMMIX_ROW_HAS_OBJ_LINK) )
         {
            pos = row[pos+ENDIAN_OBJ_NEXT_BYTE] & IMMIX_ROW_LINK_MASK;
            printf(" %d ", pos );
         }

         printf("\n");
      }

      return allocNone;
   }

   void ClearRowMarks()
   {
      unsigned char *header = mRowFlags + IMMIX_HEADER_LINES;
      unsigned char *header_end = header + IMMIX_USEFUL_LINES;
      while(header !=  header_end)
         *header++ &= IMMIX_NOT_MARKED_MASK;
   }


   // First 2 bytes are not needed for row markers (first 2 rows are for flags)
   struct
   {
      unsigned char mUsedRows;
      unsigned char mFreeInARow;
   };
   // First 2 rows contain a byte-flag-per-row 
   unsigned char  mRowFlags[IMMIX_LINES];
   // Row data as union - don't use first 2 rows
   unsigned char  mRow[IMMIX_LINES][IMMIX_LINE_LEN];
};


#define MARK_ROWS \
   unsigned char &mark = ((unsigned char *)inPtr)[ENDIAN_MARK_ID_BYTE]; \
   if ( mark==gByteMarkID  ) \
      return; \
   mark = gByteMarkID; \
 \
   register size_t ptr_i = ((size_t)inPtr)-sizeof(int); \
   unsigned int flags =  *((unsigned int *)ptr_i); \
 \
   if ( flags & (IMMIX_ALLOC_SMALL_OBJ | IMMIX_ALLOC_MEDIUM_OBJ) ) \
   { \
      char *block = (char *)(ptr_i & IMMIX_BLOCK_BASE_MASK); \
      char *base = block + ((ptr_i & IMMIX_BLOCK_OFFSET_MASK)>>IMMIX_LINE_BITS); \
      *base |= IMMIX_ROW_MARKED; \
 \
      if (flags & IMMIX_ALLOC_MEDIUM_OBJ) \
      { \
         int rows = (( (flags & IMMIX_ALLOC_SIZE_MASK) + sizeof(int) + \
                (ptr_i & (IMMIX_LINE_LEN-1)) -1 ) >> IMMIX_LINE_BITS); \
         for(int i=1;i<=rows;i++) \
            base[i] |= IMMIX_ROW_MARKED; \
      } \
   }

namespace hx
{

void MarkAlloc(void *inPtr)
{
   MARK_ROWS
}

void MarkObjectAlloc(hx::Object *inPtr)
{
   MARK_ROWS

   inPtr->__Mark();
}

}



typedef std::set<BlockData *> PointerSet;
typedef QuickVec<BlockData *> BlockList;

typedef QuickVec<unsigned int *> LargeList;

enum MemType { memUnmanaged, memBlock, memLarge };

class GlobalAllocator
{
public:
   GlobalAllocator()
   {
      mNextRecycled = 0;
      mNextEmpty = 0;
      mRowsInUse = 0;
      mLargeAllocated = 0;
      mDistributedSinceLastCollect = 0;
      // Start at 1 Meg...
      mTotalAfterLastCollect = 1<<20;
   }
   void AddLocal(LocalAllocator *inAlloc)
   {
      if (!gThreadStateChangeLock)
         gThreadStateChangeLock = new MyMutex();
      // Until we add ourselves, the colled will not wait
      //  on us - ie, we are assumed ot be in a GC free zone.
      AutoLock lock(*gThreadStateChangeLock);
      mLocalAllocs.push(inAlloc);
   }

   void RemoveLocal(LocalAllocator *inAlloc)
   {
      // You should be in the GC zone before you call this...
      AutoLock lock(*gThreadStateChangeLock);
      mLocalAllocs.qerase_val(inAlloc);
   }

   void *AllocLarge(int inSize)
   {
      inSize = (inSize +3) & ~3;
      unsigned int *result = (unsigned int *)malloc(inSize + sizeof(int)*2);
      result[0] = inSize;
      result[1] = gMarkID;

      #ifdef HXCPP_MULTI_THREADED
      bool do_lock = sMultiThreadMode;
      if (do_lock)
         mLargeListLock.Lock();
      #endif

      mLargeList.push(result);
      mLargeAllocated += inSize;
      mDistributedSinceLastCollect += inSize;

      #ifdef HXCPP_MULTI_THREADED
      if (do_lock)
         mLargeListLock.Unlock();
      #endif

      return result+2;
   }
   // Making this function "virtual" is actually a (big) performance enhancement!
   // On the iphone, sjlj (set-jump-long-jump) exceptions are used, which incur a
   //  performance overhead.  It seems that the overhead in only in routines that call
   //  malloc/new.  This is not called very often, so the overhead should be minimal.
   //  However, gcc inlines this function!  requiring every alloc the have sjlj overhead.
   //  Making it virtual prevents the overhead.
   virtual BlockData * GetRecycledBlock(int inRequiredRows)
   {
      CheckCollect();

      #ifdef HXCPP_MULTI_THREADED
      if (sMultiThreadMode)
      {
         hx::EnterGCFreeZone();
         gThreadStateChangeLock->Lock();
         hx::ExitGCFreeZoneLocked();
      }
      #endif

		BlockData *result = 0;
      if (mNextRecycled < mRecycledBlock.size())
      {
         if (mRecycledBlock[mNextRecycled]->mFreeInARow>=inRequiredRows)
         {
            result = mRecycledBlock[mNextRecycled++];
         }
         else
         {
            for(int block = mNextRecycled+1; block<mRecycledBlock.size(); block++)
            {
               if (mRecycledBlock[block]->mFreeInARow>=inRequiredRows)
               {
                  result = mRecycledBlock[block];
                  mRecycledBlock.erase(block);
               }
            }
         }

         if (result)
         {
            mDistributedSinceLastCollect +=  result->GetFreeData();
            result->ClearRecycled();
         }
      }

		if (!result)
         result = GetEmptyBlock();

      #ifdef HXCPP_MULTI_THREADED
      if (sMultiThreadMode)
         gThreadStateChangeLock->Unlock();
      #endif

      return result;
   }

   BlockData *GetEmptyBlock()
   {
      if (mNextEmpty >= mEmptyBlocks.size())
      {
         // Allocate some more blocks...
         // Using simple malloc for now, so allocate a big chuck in case we have to
         //  waste space by doing block-aligning
         char *chunk = (char *)malloc( 1<<20 );
         int n = 1<<(20-IMMIX_BLOCK_BITS);
         char *aligned = (char *)( (((size_t)chunk) + IMMIX_BLOCK_SIZE-1) & IMMIX_BLOCK_BASE_MASK);
         if (aligned!=chunk)
            n--;

         for(int i=0;i<n;i++)
         {
            BlockData *block = (BlockData *)(aligned + i*IMMIX_BLOCK_SIZE);
            block->Init();
            mAllBlocks.push(block);
            mEmptyBlocks.push(block);
         }
         // printf("Blocks %d\n", mAllBlocks.size());
      }

      BlockData *block = mEmptyBlocks[mNextEmpty++];
      block->ClearEmpty();
      mActiveBlocks.insert(block);
      mDistributedSinceLastCollect +=  block->GetFreeData();
      return block;
   }

   void ClearRowMarks()
   {
      for(PointerSet::iterator i=mActiveBlocks.begin(); i!=mActiveBlocks.end();++i)
         (*i)->ClearRowMarks();
   }

   void Collect()
   {
      LocalAllocator *this_local = 0;
      #ifdef HXCPP_MULTI_THREADED
      if (sMultiThreadMode)
      {
         hx::EnterGCFreeZone();
         gThreadStateChangeLock->Lock();
         hx::EnterGCFreeZone();
         // Someone else beat us to it ...
         if (hx::gPauseForCollect)
         {
            gThreadStateChangeLock->Unlock();
            return;
         }

         hx::gPauseForCollect = true;

         this_local = tlsLocalAlloc;
         for(int i=0;i<mLocalAllocs.size();i++)
            if (mLocalAllocs[i]!=this_local)
               WaitForSafe(mLocalAllocs[i]);
      }
      #endif

      // Now all threads have mTopOfStack & mBottomOfStack set.

      static int collect = 0;
      //printf("Collect %d\n",collect++);
      gByteMarkID = (gByteMarkID+1) & 0xff;
      gMarkID = gByteMarkID << 24;

      ClearRowMarks();

      hx::GCMarkNow();

      for(int i=0;i<mLocalAllocs.size();i++)
         MarkLocalAlloc(mLocalAllocs[i]);

      hx::RunFinalizers();

      // Reclaim ...

      sgTimeToNextTableUpdate--;
      if (sgTimeToNextTableUpdate<0)
         sgTimeToNextTableUpdate = 20;

      // Clear lists, start fresh...
      mEmptyBlocks.clear();
      mRecycledBlock.clear();
      for(PointerSet::iterator i=mActiveBlocks.begin(); i!=mActiveBlocks.end();++i)
         (*i)->Reclaim();
      mActiveBlocks.clear();
      mNextEmpty = 0;
      mNextRecycled = 0;
      mRowsInUse = 0;


      // IMMIX suggest filling up in creation order ....
      for(int i=0;i<mAllBlocks.size();i++)
      {
         BlockData *block = mAllBlocks[i];

         if (block->IsEmpty())
            mEmptyBlocks.push(block);
         else
         {
            mActiveBlocks.insert(block);
            mRowsInUse += block->GetRowsInUse();
            if (!block->IsFull())
               mRecycledBlock.push(block);
         }
      }

      int idx = 0;
      while(idx<mLargeList.size())
      {
         unsigned int *blob = mLargeList[idx];
         if ( (blob[1] & IMMIX_ALLOC_MARK_ID) != gMarkID )
         {
            mLargeAllocated -= *blob;
            free(mLargeList[idx]);
            mLargeList.qerase(idx);
         }
         else
            idx++;
      }

      mTotalAfterLastCollect = MemUsage();
      mDistributedSinceLastCollect = 0;

      #ifdef HXCPP_MULTI_THREADED
      if (sMultiThreadMode)
      {
         for(int i=0;i<mLocalAllocs.size();i++)
         if (mLocalAllocs[i]!=this_local)
            ReleaseFromSafe(mLocalAllocs[i]);

         hx::gPauseForCollect = false;
         gThreadStateChangeLock->Unlock();
      }
      #endif
   }

   void CheckCollect()
   {
      while (sgAllocInit && sgInternalEnable && mDistributedSinceLastCollect>(1<<20) &&
          mDistributedSinceLastCollect>mTotalAfterLastCollect)
      {
         //printf("Collect %d/%d\n", mDistributedSinceLastCollect, mTotalAfterLastCollect);
         Collect();
      }
   }

   size_t MemUsage()
   {
      return mLargeAllocated + (mRowsInUse<<IMMIX_LINE_BITS);
   }

   MemType GetMemType(void *inPtr)
   {
      BlockData *block = (BlockData *)( ((size_t)inPtr) & IMMIX_BLOCK_BASE_MASK);
      if ( mActiveBlocks.find(block) != mActiveBlocks.end() )
      {
         return memBlock;
      }

      for(int i=0;i<mLargeList.size();i++)
      {
         unsigned int *blob = mLargeList[i] + 2;
         if (blob==inPtr)
            return memLarge;
      }

      return memUnmanaged;
   }


   size_t mDistributedSinceLastCollect;

   size_t mRowsInUse;
   size_t mLargeAllocated;
   size_t mTotalAfterLastCollect;


   int mNextEmpty;
   int mNextRecycled;

   BlockList mAllBlocks;
   BlockList mEmptyBlocks;
   BlockList mRecycledBlock;
   LargeList mLargeList;
   PointerSet mActiveBlocks;
   MyMutex    mLargeListLock;
   QuickVec<LocalAllocator *> mLocalAllocs;
};

GlobalAllocator *sGlobalAlloc = 0;


// --- LocalAllocator -------------------------------------------------------
//
// One per thread ...

class LocalAllocator
{
public:
   LocalAllocator(int *inTopOfStack=0)
   {
      mTopOfStack = inTopOfStack;
      mGCFreeZone = false;
      Reset();
      mState = lasNew;
      sGlobalAlloc->AddLocal(this);
      mState = lasRunning;
      #ifdef _MSC_VER
      mID = GetCurrentThreadId();
      #endif
   }

   ~LocalAllocator()
   {
      mState = lasTerminal;
      EnterGCFreeZone();
      sGlobalAlloc->RemoveLocal(this);
   }

   void Reset()
   {
      mCurrent = 0;
      mOverflow = 0;
      mCurrentLine = IMMIX_LINES;
      mCurrentPos = 0;
      mLinesSinceLastCollect = 0; 
   }

   void SetTopOfStack(int *inTop)
   {
      // stop early to allow for ptr[1] ....
      if (inTop>mTopOfStack)
         mTopOfStack = inTop;
   }

   void SetBottomOfStack(int *inBottom)
   {
      mBottomOfStack = inBottom;
   }


   void PauseForCollect()
   {
      int dummy = 1;
      mBottomOfStack = &dummy;
      mReadyForCollect.Set();
      mCollectDone.Wait();
   }

   void EnterGCFreeZone()
   {
      int dummy = 1;
      mBottomOfStack = &dummy;
      mGCFreeZone = true;
      mReadyForCollect.Set();
   }

   void ExitGCFreeZone()
   {
      if (sMultiThreadMode)
      {
         AutoLock lock(*gThreadStateChangeLock);
         mReadyForCollect.Reset();
         mGCFreeZone = false;
      }
   }
        // For when we already hold the lock
   void ExitGCFreeZoneLocked()
   {
      if (sMultiThreadMode)
      {
         mReadyForCollect.Reset();
         mGCFreeZone = false;
      }
   }



   // Called by the collecting thread to make sure this allocator is paused.
   // The collecting thread has the lock, and will not be releasing it until
   //  it has finished the collect.
   void WaitForSafe()
   {
      if (!mGCFreeZone)
         mReadyForCollect.Wait();
   }

   void ReleaseFromSafe()
   {
      if (!mGCFreeZone)
         mCollectDone.Set();
   }



   void *Alloc(int inSize,bool inIsObject)
   {
      #ifdef HXCPP_MULTI_THREADED
      if (hx::gPauseForCollect)
         PauseForCollect();
      #endif

      inSize = (inSize + 3 ) & ~3;

      int s = inSize +sizeof(int);
      // Try to squeeze it on this line ...
      if (mCurrentPos > 0)
      {
         int skip = 1;
         int extra_lines = (s + mCurrentPos-1) >> IMMIX_LINE_BITS;

         #if 0
         // Optimise for fitting on same line...
         if (!extra_lines)
         {
            unsigned char *row = mCurrent->mRow[mCurrentLine];
            unsigned char &row_flag = mCurrent->mRowFlags[mCurrentLine];

            int *result = (int *)(row + mCurrentPos);
            if (inIsObject)
               *result = inSize | gMarkID | (row_flag<<16) |
                  (IMMIX_ALLOC_SMALL_OBJ | IMMIX_ALLOC_IS_OBJECT); 
            else
               *result = inSize | gMarkID | (row_flag<<16) | IMMIX_ALLOC_SMALL_OBJ; 

            row_flag =  mCurrentPos | IMMIX_ROW_HAS_OBJ_LINK;

            mCurrentLine += extra_lines;
            mCurrentPos = (mCurrentPos + s) & (IMMIX_LINE_LEN-1);
            if (mCurrentPos==0)
               mCurrentLine++;

            //printf("Alloced %d - %d/%d now\n", s, mCurrentPos, mCurrentLine);
            return result + 1;
         }
         #endif


         //printf("check for %d extra lines ...\n", extra_lines);
         if (mCurrentLine + extra_lines < IMMIX_LINES)
         {
            int test = 0;
            if (extra_lines)
            {
               const unsigned char *used = mCurrent->mRowFlags + mCurrentLine+test+1;
               for(test=0;test<extra_lines;test++)
                  if ( used[test] & IMMIX_ROW_MARKED)
                     break;
            }
            //printf(" found %d extra lines\n", test);
            if (test==extra_lines)
            {
               // Ok, fits on the line! - setup object table
               unsigned char *row = mCurrent->mRow[mCurrentLine];
               unsigned char &row_flag = mCurrent->mRowFlags[mCurrentLine];

               int *result = (int *)(row + mCurrentPos);
               *result = inSize | gMarkID |
                  (row_flag<<16) |
                  (extra_lines==0 ? IMMIX_ALLOC_SMALL_OBJ : IMMIX_ALLOC_MEDIUM_OBJ );

               if (inIsObject)
                  *result |= IMMIX_ALLOC_IS_OBJECT;

               row_flag =  mCurrentPos | IMMIX_ROW_HAS_OBJ_LINK;

               mCurrentLine += extra_lines;
               mCurrentPos = (mCurrentPos + s) & (IMMIX_LINE_LEN-1);
               if (mCurrentPos==0)
                  mCurrentLine++;

               //printf("Alloced %d - %d/%d now\n", s, mCurrentPos, mCurrentLine);

               return result + 1;
            }
            //printf("not enought extra lines - skip %d\n",skip);
            skip = test + 1;
         }
         else
            skip = extra_lines;

         // Does not fit on this line - we may also know how many to skip, so
         //  jump down those lines...
         mCurrentPos = 0;
         mCurrentLine += skip;
      }

      int required_rows = (s + IMMIX_LINE_LEN-1) >> IMMIX_LINE_BITS;
      int last_start = IMMIX_LINES - required_rows;

      while(1)
      {
         // Alloc new block, if required ...
         if (!mCurrent || mCurrentLine>last_start)
         {
            int dummy = 1;
            mBottomOfStack = &dummy;
            mCurrent = sGlobalAlloc->GetRecycledBlock(required_rows);
            //mCurrent->Verify();
            // Start on line 2 (there are 256 line-markers at the beginning)
            mCurrentLine = IMMIX_HEADER_LINES;
         }

         // Look for N in a row ....
         while(mCurrentLine <= last_start)
         {
            int test = 0;
            const unsigned char *used = mCurrent->mRowFlags + mCurrentLine+test;
            for(;test<required_rows;test++)
               if ( used[test] & IMMIX_ROW_MARKED)
                  break;

            // Not enough room...
            if (test<required_rows)
            {
               mCurrentLine += test+1;
               //printf("  Only found %d good - skip to %d\n",test,mCurrentLine);
               continue;
            }

            // Ok, found a gap
            unsigned char *row = mCurrent->mRow[mCurrentLine];

            int *result = (int *)(row + mCurrentPos);
            *result = inSize | gMarkID |
               (required_rows==1 ? IMMIX_ALLOC_SMALL_OBJ : IMMIX_ALLOC_MEDIUM_OBJ );

            if (inIsObject)
               *result |= IMMIX_ALLOC_IS_OBJECT;

            mCurrent->mRowFlags[mCurrentLine] = mCurrentPos | IMMIX_ROW_HAS_OBJ_LINK;

            //mCurrent->DirtyLines(mCurrentLine,required_rows);
            mCurrentLine += required_rows - 1;
            mCurrentPos = (mCurrentPos + s) & (IMMIX_LINE_LEN-1);
            if (mCurrentPos==0)
               mCurrentLine++;

            return result + 1;
         }
      }
      return 0;
   }


   void Mark()
   {
      int here = 0;
      void *prev = 0;
      // printf("=========== Mark Stack ==================== %p/%d\n",mBottomOfStack,&here);
      for(int *ptr = mBottomOfStack ; ptr<mTopOfStack; ptr++)
      {
         void *vptr = *(void **)ptr;
         MemType mem;
         if (vptr && !((size_t)vptr & 0x03) && vptr!=prev &&
                 (mem = sGlobalAlloc->GetMemType(vptr)) != memUnmanaged )
         {
            if (mem==memLarge)
            {
               unsigned char &mark = ((unsigned char *)(vptr))[-1];
               if (mark!=gByteMarkID)
                  mark = gByteMarkID;
            }
            else
            {
               BlockData *block = (BlockData *)( ((size_t)vptr) & IMMIX_BLOCK_BASE_MASK);
               int pos = (int)(((size_t)vptr) & IMMIX_BLOCK_OFFSET_MASK);
               AllocType t = block->GetAllocType(pos-sizeof(int),true);
               if ( t==allocObject )
               {
                  // printf(" Mark object %p (%p)\n", vptr,ptr);
                  HX_MARK_OBJECT( ((hx::Object *)vptr) );
               }
               else if (t==allocString)
               {
                  // printf(" Mark string %p (%p)\n", vptr,ptr);
                  HX_MARK_STRING(vptr);
               }
            }
         }
      }
      //printf("marked\n");
      Reset();
   }

   int mCurrentPos;
   int mCurrentLine;

   int mOverflowPos;
   int mOverflowLine;

   int mLinesSinceLastCollect;

   BlockData * mCurrent;
   BlockData * mOverflow;

   int *mTopOfStack;
   int *mBottomOfStack;

   bool            mGCFreeZone;
   int             mID;
   LocalAllocState mState;
   MySemaphore     mReadyForCollect;
   MySemaphore     mCollectDone;
};

LocalAllocator *sMainThreadAlloc = 0;


LocalAllocator *GetLocalAllocMT()
{
   LocalAllocator *result =  tlsLocalAlloc.Get();
   if (!result)
   {
      result = new LocalAllocator();
      tlsLocalAlloc.Set(result);
   }
   return result;
}


inline LocalAllocator *GetLocalAlloc()
{
   #ifdef HXCPP_MULTI_THREADED
   if (sMultiThreadMode)
      return GetLocalAllocMT();
   #endif
   return sMainThreadAlloc;
}

void WaitForSafe(LocalAllocator *inAlloc)
{
   inAlloc->WaitForSafe();
}

void ReleaseFromSafe(LocalAllocator *inAlloc)
{
   inAlloc->ReleaseFromSafe();
}

void MarkLocalAlloc(LocalAllocator *inAlloc)
{
   inAlloc->Mark();
}


namespace hx
{

void PauseForCollect()
{
   GetLocalAlloc()->PauseForCollect();
}



void EnterGCFreeZone()
{
   if (sMultiThreadMode)
   {
      LocalAllocator *tla = GetLocalAlloc();
      tla->EnterGCFreeZone();
   }
}

void ExitGCFreeZone()
{
   if (sMultiThreadMode)
   {
      LocalAllocator *tla = GetLocalAlloc();
      tla->ExitGCFreeZone();
   }
}

void ExitGCFreeZoneLocked()
{
   if (sMultiThreadMode)
   {
      LocalAllocator *tla = GetLocalAlloc();
      tla->ExitGCFreeZoneLocked();
   }
}

void InitAlloc()
{
   sgAllocInit = true;
   sGlobalAlloc = new GlobalAllocator();
   sgFinalizers = new FinalizerList();
   hx::Object tmp;
   void **stack = *(void ***)(&tmp);
   sgObject_root = stack[0];
   //printf("__root pointer %p\n", sgObject_root);
   sMainThreadAlloc =  new LocalAllocator();

}

void SetTopOfStack(int *inTop)
{
   if (!sgAllocInit)
      InitAlloc();

   LocalAllocator *tla = GetLocalAlloc();

   sgInternalEnable = true;

   return tla->SetTopOfStack(inTop);

}


void GCPrepareMultiThreaded()
{
   if (!sMultiThreadMode)
   {
      sMultiThreadMode = true;
      tlsLocalAlloc = sMainThreadAlloc;
   }
}



void *InternalNew(int inSize,bool inIsObject)
{
   if (!sgAllocInit)
      InitAlloc();

   if (inSize>=IMMIX_LARGE_OBJ_SIZE)
      return sGlobalAlloc->AllocLarge(inSize);
   else
   {
      LocalAllocator *tla = GetLocalAlloc();
      return tla->Alloc(inSize,inIsObject);
   }
}

// Force global collection - should only be called from 1 thread.
void InternalCollect()
{
   if (!sgAllocInit || !sgInternalEnable)
      return;

   int dummy;
   GetLocalAlloc()->SetTopOfStack(&dummy);
   sGlobalAlloc->Collect();
}


void *InternalRealloc(void *inData,int inSize)
{
   if (inData==0)
      return hx::InternalNew(inSize,false);

   unsigned int header = ((unsigned int *)(inData))[-1];

   unsigned int s = (header & ( IMMIX_ALLOC_SMALL_OBJ | IMMIX_ALLOC_MEDIUM_OBJ)) ?
         (header & IMMIX_ALLOC_SIZE_MASK) :  ((unsigned int *)(inData))[-2];

   void *new_data = 0;

   if (inSize>=IMMIX_LARGE_OBJ_SIZE)
   {
      new_data = sGlobalAlloc->AllocLarge(inSize);
      if (inSize>s)
         memset((char *)new_data + s,0,inSize-s);
   }
   else
   {
      LocalAllocator *tla = GetLocalAlloc();

      new_data = tla->Alloc(inSize,false);
   }

   int min_size = s < inSize ? s : inSize;

   memcpy(new_data, inData, min_size );

   return new_data;
}



void RegisterCurrentThread(void *inTopOfStack)
{
   // Create a local-alloc
   LocalAllocator *local = new LocalAllocator((int *)inTopOfStack);
   tlsLocalAlloc = local;
}

void UnregisterCurrentThread()
{
   LocalAllocator *local = tlsLocalAlloc;
   delete local;
   tlsLocalAlloc = 0;
}


} // end namespace hx

int   __hxcpp_gc_used_bytes()
{
   return sGlobalAlloc->MemUsage();
}

extern "C"
{
void hxcpp_set_top_of_stack()
{
   int i = 0;
   hx::SetTopOfStack(&i);
}
}

#endif // if HX_INTERNAL_GC