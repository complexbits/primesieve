///
/// @file   CpuInfo.cpp
/// @brief  Get the CPUs' cache sizes in bytes
///
/// Copyright (C) 2018 Kim Walisch, <kim.walisch@gmail.com>
///
/// This file is distributed under the BSD License. See the COPYING
/// file in the top level directory.
///

#include <primesieve/CpuInfo.hpp>
#include <primesieve/pmath.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#if defined(__APPLE__)
  #if !defined(__has_include)
    #define APPLE_SYSCTL
  #elif __has_include(<sys/types.h>) && \
        __has_include(<sys/sysctl.h>)
    #define APPLE_SYSCTL
  #endif
#endif

#if defined(_WIN32)

#include <windows.h>
#include <vector>

#elif defined(APPLE_SYSCTL)

#include <sys/types.h>
#include <sys/sysctl.h>
#include <vector>

#else // all other OSes

#include <fstream>
#include <sstream>

using namespace std;

namespace {

string getString(const string& filename)
{
  ifstream file(filename);
  string str;

  if (file)
  {
    // https://stackoverflow.com/a/3177560/363778
    stringstream trimmer;
    trimmer << file.rdbuf();
    trimmer >> str;
  }

  return str;
}

size_t getValue(const string& filename)
{
  size_t val = 0;
  string str = getString(filename);

  if (!str.empty())
  {
    val = stol(str);

    // Last character may be:
    // 'K' = kilobytes
    // 'M' = megabytes
    // 'G' = gigabytes
    if (str.back() == 'K')
      val *= 1024;
    if (str.back() == 'M')
      val *= 1024 * 1024;
    if (str.back() == 'G')
      val *= 1024 * 1024 * 1024;
  }

  return val;
}

vector<string> split(const string& s, char delimiter)
{
   vector<string> tokens;
   string token;
   istringstream tokenStream(s);

   while (getline(tokenStream, token, delimiter))
      tokens.push_back(token);

   return tokens;
}

} // namespace

#endif

using namespace std;

namespace primesieve {

CpuInfo::CpuInfo()
  : l1CacheSize_(0),
    l2CacheSize_(0),
    l2Threads_(0),
    threadsPerCore_(0)
{
  try
  {
    init();
  }
  catch (exception& e)
  {
    error_ = e.what();
  }
}

size_t CpuInfo::l1CacheSize() const
{
  return l1CacheSize_;
}

size_t CpuInfo::l2CacheSize() const
{
  return l2CacheSize_;
}

size_t CpuInfo::l2Threads() const
{
  return l2Threads_;
}

string CpuInfo::getError() const
{
  return error_;
}

bool CpuInfo::hasL1Cache() const
{
  return l1CacheSize_ >= (1 << 12) &&
         l1CacheSize_ <= (1 << 30);
}

bool CpuInfo::hasL2Cache() const
{
  return l2CacheSize_ >= (1 << 12) &&
         l2CacheSize_ <= (1 << 30);
}

bool CpuInfo::privateL2Cache() const
{
  return l2Threads_ >= 1 &&
         l2Threads_ <= (1 << 10) &&
         l2Threads_ <= threadsPerCore_;
}

bool CpuInfo::hasHyperThreading() const
{
  return l2Threads_ >= 2 &&
         l2Threads_ <= (1 << 10) &&
         l2Threads_ <= threadsPerCore_;
}

#if defined(APPLE_SYSCTL)

void CpuInfo::init()
{
  size_t l1Length = sizeof(l1CacheSize_);
  size_t l2Length = sizeof(l2CacheSize_);

  sysctlbyname("hw.l1dcachesize", &l1CacheSize_, &l1Length, NULL, 0);
  sysctlbyname("hw.l2cachesize" , &l2CacheSize_, &l2Length, NULL, 0);

  size_t size = 0;

  if (!sysctlbyname("hw.cacheconfig", NULL, &size, NULL, 0))
  {
    size_t n = size / sizeof(size);
    vector<size_t> cacheconfig(n);

    if (cacheconfig.size() > 2)
    {
      // https://developer.apple.com/library/content/releasenotes/Performance/RN-AffinityAPI/index.html
      sysctlbyname("hw.cacheconfig" , &cacheconfig[0], &size, NULL, 0);
      l2Threads_ = cacheconfig[2];

      size_t physicalcpu = 1;
      size = sizeof(size);
      sysctlbyname("hw.physicalcpu", &physicalcpu, &size, NULL, 0);
      physicalcpu = max<size_t>(1, physicalcpu);

      size_t logicalcpu = 1;
      size = sizeof(size);
      sysctlbyname("hw.logicalcpu", &logicalcpu, &size, NULL, 0);
      threadsPerCore_ = logicalcpu / physicalcpu;
    }
  }
}

#elif defined(_WIN32)

void CpuInfo::init()
{
  typedef BOOL (WINAPI *LPFN_GLPI)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

  LPFN_GLPI glpi = (LPFN_GLPI) GetProcAddress(GetModuleHandle(TEXT("kernel32")), "GetLogicalProcessorInformation");

  if (!glpi)
    return;

  DWORD bytes = 0;
  glpi(0, &bytes);

  if (!bytes)
    return;

  size_t size = bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
  vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(size);

  if (!glpi(&info[0], &bytes))
    return;

  for (size_t i = 0; i < size; i++)
  {
    if (info[i].Relationship == RelationProcessorCore)
    {
      auto mask = info[i].ProcessorMask;

      // ProcessorMask contains one bit set for
      // each logical CPU core related to the
      // current physical CPU core
      for (threadsPerCore_ = 0; mask > 0; threadsPerCore_++)
        mask &= mask - 1;
    }

    if (info[i].Relationship == RelationCache &&
        (info[i].Cache.Type == CacheData ||
         info[i].Cache.Type == CacheUnified))
    {
      if (info[i].Cache.Level == 1)
        l1CacheSize_ = info[i].Cache.Size;
      if (info[i].Cache.Level == 2)
        l2CacheSize_ = info[i].Cache.Size;

      // if the CPU has an L3 cache we assume
      // the L2 cache is private
      if (info[i].Cache.Level == 3 &&
          info[i].Cache.Size > 0)
        l2Threads_ = threadsPerCore_;
    }
  }

// Windows 7 (2009) or later
#if _WIN32_WINNT >= 0x0601

  typedef BOOL (WINAPI *LPFN_GLPIEX)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);

  LPFN_GLPIEX glpiex = (LPFN_GLPIEX) GetProcAddress(
      GetModuleHandle(TEXT("kernel32")), "GetLogicalProcessorInformationEx");

  if (!glpiex)
    return;

  bytes = 0;
  glpiex(RelationAll, 0, &bytes);

  if (!bytes)
    return;

  vector<char> buffer(bytes);
  SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* cpu;

  if (!glpiex(RelationAll, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*) &buffer[0], &bytes))
    return;

  for (size_t i = 0; i < bytes; i += cpu->Size)
  {
    cpu = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*) &buffer[i];

    // check L2 cache
    if (cpu->Relationship == RelationCache &&
        cpu->Cache.GroupMask.Group == 0 &&
        cpu->Cache.Level == 2 &&
        (cpu->Cache.Type == CacheData ||
         cpu->Cache.Type == CacheUnified))
    {
      // @warning: GetLogicalProcessorInformationEx() reports
      // incorrect data when Windows is run inside a virtual
      // machine. Specifically the GROUP_AFFINITY.Mask will
      // only have 1 or 2 bits set for each CPU cache (L1, L2 and
      // L3) even if more logical CPU cores share the cache
      auto mask = cpu->Cache.GroupMask.Mask;
      l2Threads_ = 0;

      // Cache.GroupMask.Mask contains one bit set for
      // each logical CPU core sharing the cache
      for (; mask > 0; l2Threads_++)
        mask &= mask - 1;

      break;
    }
  }

#endif
}

#else

/// This works on Linux and Android. We also use this
/// for all unknown OSes, it might work.
///
void CpuInfo::init()
{
  for (int i = 0; i <= 3; i++)
  {
    string filename = "/sys/devices/system/cpu/cpu0/cache/index" + to_string(i);
    string threadSiblingsList = "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list";

    string cacheLevel = filename + "/level";
    string cacheSize = filename + "/size";
    string sharedCpuList = filename + "/shared_cpu_list";
    string cacheType = filename + "/type";

    size_t level = getValue(cacheLevel);
    string type = getString(cacheType);
    threadSiblingsList = getString(threadSiblingsList);
    threadsPerCore_ = 0;

    // Parse the thread_siblings_list
    // content can be either: 
    // 1) threadId1, threadId2, ...
    // 2) threadId1-threadId2, ...
    for (auto& str : split(threadSiblingsList, ','))
    {
      if (str.find('-') == string::npos)
        threadsPerCore_++;
      else
      {
        auto values = split(str, '-');
        auto threadId0 = stoi(values.at(0));
        auto threadId1 = stoi(values.at(1));
        threadsPerCore_ += threadId1 - threadId0 + 1;
      }
    }

    if (level == 1 &&
        (type == "Data" ||
         type == "Unified"))
    {
      l1CacheSize_ = getValue(cacheSize);
    }

    if (level == 2 &&
        (type == "Data" ||
         type == "Unified"))
    {
      l2CacheSize_ = getValue(cacheSize);
      sharedCpuList = getString(sharedCpuList);

      // https://lwn.net/Articles/254445/
      if (!sharedCpuList.empty() &&
          sharedCpuList == threadSiblingsList)
        l2Threads_ = threadsPerCore_;
    }
  }
}

#endif

/// Singleton
const CpuInfo cpuInfo;

} // namespace
