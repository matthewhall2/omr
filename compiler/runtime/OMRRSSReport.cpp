/*******************************************************************************
 * Copyright IBM Corp. and others 2000
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/
#include "compiler/infra/String.hpp"
#include "runtime/OMRRSSReport.hpp"

OMR::RSSReport *OMR::RSSReport::_instance = NULL;

const char *OMR::RSSItem::itemNames [] =
      {
      "Unknown",
      "HEADER",
      "ALIGNMENT",
      "COLD BLOCKS",
      "OVERESTIMATE",
      "OOL",
      "SNIPPET"
      };

void
OMR::RSSRegion::addRSSItem(OMR::RSSItem *item, int32_t threadId, const char* methodName)
   {
#ifdef DEBUG_RSS_REPORT
   TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS adding item addr=%p size=%zu type=%s thread=%d method=%s", item->_addr, item->_size,
                                  OMR::RSSItem::itemNames[item->_type], threadId,
                                  methodName ? methodName : "no method");
#endif

   uint8_t *address = item->_addr;
   TR_PersistentList<TR::DebugCounterAggregation> *counters = item->_counters;

   TR_ASSERT_FATAL(address, "Address should not be null");
   TR_ASSERT_FATAL(_pageSize > 0, "Page size should be set");

   size_t addressPage = (uintptr_t)address / _pageSize;
   size_t startPage = (uintptr_t)_start / _pageSize;
   int32_t offset = static_cast<int32_t>((_grows == lowToHigh) ? addressPage - startPage : startPage - addressPage);

   TR_ASSERT_FATAL(offset >= 0, "Offset should be >= 0\n");

   size_t tillPageEnd = _pageSize - ((uintptr_t)address % _pageSize);
   size_t remainingSize = item->_size;

   if (remainingSize > tillPageEnd)
      item->_size = tillPageEnd;

   addToListSorted(&_pageMap[offset], item);

   remainingSize -= item->_size;
   address += tillPageEnd;  // next address in case it's needed, points to the beginning of the next page

   while (remainingSize > 0)
      {
      size_t  newSize = remainingSize;

      if (newSize > _pageSize)
         newSize = _pageSize;

      OMR::RSSItem *newItem = new (TR::Compiler->persistentMemory()) OMR::RSSItem(item->_type, address, newSize, counters);
      offset = (_grows == lowToHigh) ? ++offset : --offset;

      TR_ASSERT_FATAL(offset >= 0, "Got negative offset %d for addr=%p size=%zu type=%s\n", offset,
                      newItem->_addr, newItem->_size, OMR::RSSItem::itemNames[newItem->_type]);

#ifdef DEBUG_RSS_REPORT
      TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS adding overflow item addr=%p size=%zu type=%s thread=%d method=%s",
                                     newItem->_addr, newItem->_size,
                                     OMR::RSSItem::itemNames[newItem->_type], threadId,
                                     methodName ? methodName : "no method");
#endif

      addToListSorted(&_pageMap[offset], newItem);
      address += _pageSize;
      remainingSize -= newSize;
      }
   }

void
OMR::RSSRegion::addToListSorted(TR_PersistentList<OMR::RSSItem> *itemList, OMR::RSSItem *item)
   {
   ListElement<OMR::RSSItem> *prevElement = NULL;
   ListElement<OMR::RSSItem> *newElement = NULL;
   uint8_t *itemStart = item->_addr;
   uint8_t *itemEnd = item->_addr + item->_size;
   OMR::RSSItem *next = NULL;

   ListIterator<OMR::RSSItem> it(itemList);

   for (next = it.getFirst(); next; next = it.getNext())
      {
      uint8_t *nextEnd = next->_addr + next->_size;

      if (itemStart < nextEnd)
         {
         newElement = itemList->addAfter(item, prevElement);
         break;
         }

      prevElement = it.getCurrentElement();
      }

   if (!next)
      newElement = itemList->addAfter(item, prevElement);

   // Remove all subsequent overlaping items
   // Due to the way we inserted the item above, all ovelaping items will be next
   //
   for (ListElement<OMR::RSSItem> * nextElement = newElement->getNextElement(); nextElement; nextElement = nextElement->getNextElement())
      {
      OMR::RSSItem *nextItem = nextElement->getData();
      uint8_t *nextStart = nextItem->_addr;
      uint8_t *nextEnd = nextStart + nextItem->_size;

      if (itemStart >= nextStart || itemEnd > nextStart)
         {
         itemList->removeNext(newElement);
         }
      else
         {
         break;
         }
      }
   }

#ifdef LINUX
typedef struct __attribute__ ((__packed__))
   {
   union
      {
      uint64_t _pmd; // page memory descriptor
      uint64_t _pageFrameNumber : 55; // bits 0..54 represent the page frame number (PFN) if present
                                      // if not present, they represent the swap type and offset
      struct
         {
         uint64_t _swapType: 5;    // swap type if swapped
         uint64_t _swapOffset: 50; // swap offset if swapped
         uint64_t _softDirty: 1;   // bit 55: pte is soft dirty
         uint64_t _exclusive: 1;   // bit 56: page exclussively mapped
         uint64_t _wp:1;           // bit 57: pte is uffd-wp write-protected (since 5.13)
         uint64_t _zero: 3;        // bits 58..60 are zero
         uint64_t _filePage: 1;    // bit 61: page is file-page or shared-anon
         uint64_t _swapped: 1;     // bit 62: page is swapped
         uint64_t _present: 1;     // bit 63: page is present
         };
       };
   } pmd_t;
#endif

size_t
OMR::RSSReport::countResidentPages(int fd, OMR::RSSRegion *rssRegion, char *pagesInfo)
   {
   size_t  rss = 0;

#ifdef LINUX
   uint8_t *start = rssRegion->_start;
   size_t size = rssRegion->_size;

   if (rssRegion->_grows == OMR::RSSRegion::highToLow)
      start = start - size;

   uint8_t *end = start + size;
   size_t  pageCount = 0;
   size_t  pageSize = rssRegion->_pageSize;

   TR_ASSERT_FATAL(pageSize >=0, "Page size should be set");

   for (uint8_t *addr = start; addr < end; addr += pageSize)
      {
      pmd_t pmd;
      uint64_t index = ((uint64_t)addr / pageSize) * sizeof(pmd._pmd);

      if (pread(fd, &pmd._pmd, sizeof(pmd._pmd), index) != sizeof(pmd))
         {
         perror("cannot read from pagemap file");
         break;
         }

      size_t resident = pmd._present;
      rss += resident;

      if (!_detailed) continue;

      // Print RSS items and debug counters for each resident page
      //
#ifdef DEBUG_RSS_REPORT // printing RSS bit for every page is rarely used
      if (pagesInfo)
         {
         size_t offset = pageCount * PAGE_PRINT_WIDTH;

         if ((offset + PAGE_PRINT_WIDTH + 1) < MAX_RSS_LINE)
            {
            // Note: In addition to the pafgesInfo limit, TR_VerboseLog will truncate the line to some maximum
            // PAGE_PRINT_WIDTH+1 ensures each field will not take more than expected by the check above
            //
            TR::snprintfTrunc(pagesInfo + offset, PAGE_PRINT_WIDTH+1, "%1zu ", resident);
            }
          else
            {
            TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS: The following line is incomplete");
            }
         }
#endif

      TR_PersistentList<OMR::RSSItem> itemList = rssRegion->getRSSItemList(addr);

      if (resident == 1 &&
          itemList.isEmpty())
         {
         TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS: Resident page at addr %p has no RSS items", addr);
         }
      else if (resident == 1)
         {
         ListIterator<OMR::RSSItem> it(&itemList);

         size_t totalItemSize = 0;
         int32_t countItems = 0;
         uint8_t *prevEnd = (addr == start) ? addr : (uint8_t *)(((uintptr_t)addr / pageSize) * pageSize);  // current page start
         OMR::RSSItem *prevItem = NULL;

         uint64_t pageDebugCount = 0;

         // print RSS items
         for (OMR::RSSItem *item = it.getFirst(); item; item = it.getNext())
            {
            uint64_t itemDebugCount = 0;

            if (item->_counters)
               {
               ListIterator<TR::DebugCounterAggregation> aggrIt(item->_counters);

               // print debug counters
               for (TR::DebugCounterAggregation *aggr = aggrIt.getFirst(); aggr; aggr = aggrIt.getNext())
                  {
                  uint64_t count = aggr->getCount();

                  itemDebugCount += count;
                  pageDebugCount += count;
                  if (count > 0) aggr->printCounters(false /*printZeroCounters*/);
                  }
               }

            int32_t gap = item->_addr - prevEnd;

            TR_ASSERT_FATAL(gap >= 0, "Item at addr %p in page %p is out of order, prevAddr=%p", item->_addr, addr,
                            prevItem->_addr);

            if (gap != 0)
               {
               TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS: gap at addr=%p size=%zu in %s region", prevEnd, gap, rssRegion->_name);
               }

            totalItemSize += item->_size + gap;

            TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS item at addr=%p size=%zu itemDebugCount=%zu %s",
                                           item->_addr, item->_size, itemDebugCount, OMR::RSSItem::itemNames[item->_type]);

            countItems++;
            prevEnd = item->_addr + item->_size;
            prevItem = item;
            }

         TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS: Page at addr %p has %zu bytes of %d items pageDebugCount=%zu",
                                        addr, totalItemSize, countItems, pageDebugCount);

         TR_ASSERT_FATAL(totalItemSize <= pageSize, "Total size of items within page %p > page size\n", addr);
         }

      pageCount++;
      }
#endif

   return rss;
   }

void
OMR::RSSReport::printTitle()
   {
   int32_t regionNum = 0;
   char rssLine[MAX_RSS_LINE];

   ListIterator<OMR::RSSRegion> it(&_regions);
   for (OMR::RSSRegion *rssRegion = it.getFirst(); rssRegion; rssRegion = it.getNext())
      {
      size_t offset = regionNum * REGION_PRINT_WIDTH;

      if ((offset + REGION_PRINT_WIDTH + 1) < MAX_RSS_LINE)
         {
         // Note: In addition to the rssLine limit, TR_VerboseLog will truncate the line to some maximum
         // REGION_PRINT_WIDTH+1 ensures each field will not take more than expected by the check above
         //
         TR::snprintfTrunc(rssLine + offset, REGION_PRINT_WIDTH+1, "%*.*s", REGION_PRINT_WIDTH, REGION_PRINT_WIDTH, rssRegion->_name);
         }
      else
         {
         break;
         }
      regionNum++;
      }

   TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS Region name:    %s", rssLine);

   it.reset();
   regionNum = 0;
   for (OMR::RSSRegion *rssRegion = it.getFirst(); rssRegion; rssRegion = it.getNext())
      {
      size_t offset = regionNum * REGION_PRINT_WIDTH;

      if ((offset + REGION_PRINT_WIDTH + 1) < MAX_RSS_LINE)
         {
         // Note: In addition to the rssLine limit, TR_VerboseLog will truncate the line to some maximum
         // REGION_PRINT_WIDTH+1 ensures each field will not take more than expected by the check above
         //
         TR::snprintfTrunc(rssLine + offset, REGION_PRINT_WIDTH+1, "%*p", REGION_PRINT_WIDTH, rssRegion->_start);
         }
      else
         {
         break;
         }

      regionNum++;
      }

   TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS Region start:     %s", rssLine);
   _printTitle = false;
   }

void
OMR::RSSReport::printRegions()
   {
   size_t totalKb = 0;
   size_t totalRSSKb = 0;
   int32_t regionNum = 0;
   char rssLine[MAX_RSS_LINE];

#ifdef DEBUG_RSS_REPORT
   char pagesInfo[MAX_RSS_LINE];
#else
   char *pagesInfo = NULL;
#endif

   static const char *pagemapPath = "/proc/self/pagemap";
   int fd = 0;

#ifdef LINUX
   fd = open(pagemapPath, O_RDONLY);
   if (fd < 0)
      {
      perror("cannot open pagemap file");
      return;
      }
#endif

   if (_printTitle) printTitle();

   ListIterator<OMR::RSSRegion> it(&_regions);

   for (OMR::RSSRegion *rssRegion = it.getFirst(); rssRegion; rssRegion = it.getNext())
      {
      size_t pageSize = rssRegion->_pageSize;

      TR_ASSERT_FATAL(pageSize >=0, "Page size should be set");

      size_t rssPages = countResidentPages(fd, rssRegion, pagesInfo);
      size_t rssSizeKb  = rssPages * pageSize / 1024;
      size_t sizeKb  = rssRegion->_size / 1024;
      size_t offset = regionNum * REGION_PRINT_WIDTH;

      if ((offset + REGION_PRINT_WIDTH + 1) < MAX_RSS_LINE)
         {
         // Note: In addition to the rssLine limit, TR_VerboseLog will truncate the line to some maximum
         // REGION_PRINT_WIDTH+1 ensures each field will not take more than expected by the check above
         //
         TR::snprintfTrunc(rssLine + offset, REGION_PRINT_WIDTH+1, " %5zu/%5zu(%3.0f%%)", rssSizeKb, sizeKb, (float)rssSizeKb*100.0/sizeKb);
         }

      totalKb += sizeKb;
      totalRSSKb += rssSizeKb;
      regionNum++;
      }

#ifdef LINUX
   close(fd);
#endif

   TR_VerboseLog::writeLineLocked(TR_Vlog_PERF, "RSS Region rss/size:    %s  all %u regions %5zu/%5zu(%3.0f%%) KB", rssLine, regionNum, totalRSSKb, totalKb, (float)totalRSSKb*100.0/totalKb);
   }
