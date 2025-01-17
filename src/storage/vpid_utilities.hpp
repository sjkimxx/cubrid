/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#ifndef _VPID_UTILS_HPP_
#define _VPID_UTILS_HPP_

#include "dbtype_def.h"

#include <cstdint>
#include <cstdio>
#include <functional>

namespace std
{
  template<>
  struct less<VPID>
  {
    bool operator () (const VPID &lhs, const VPID &rhs) const
    {
      if (lhs.volid != rhs.volid)
	{
	  return lhs.volid < rhs.volid;
	}
      else
	{
	  return lhs.pageid < rhs.pageid;
	}
    }
  };

  template<>
  struct hash<VPID>
  {
    inline std::size_t operator () (const VPID &val) const noexcept
    {
      std::size_t res = 0;
      res ^= std::hash<short> {} (val.volid) + 0x9e3779b9 + (res << 6) + (res >> 2);
      res ^= std::hash<int32_t> {} (val.pageid) + 0x9e3779b9 + (res << 6) + (res >> 2);
      return res;
    }
  };
}

#endif // _VPID_UTILS_HPP_
