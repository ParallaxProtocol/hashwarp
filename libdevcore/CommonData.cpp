/*
    This file is part of ethminer.

    ethminer is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ethminer is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cmath>
#include <cstdlib>

#include "CommonData.h"
#include "Exceptions.h"

using namespace std;
using namespace dev;

int dev::fromHex(char _i, WhenError _throw)
{
    if (_i >= '0' && _i <= '9')
        return _i - '0';
    if (_i >= 'a' && _i <= 'f')
        return _i - 'a' + 10;
    if (_i >= 'A' && _i <= 'F')
        return _i - 'A' + 10;
    if (_throw == WhenError::Throw)
        BOOST_THROW_EXCEPTION(BadHexCharacter() << errinfo_invalidSymbol(_i));
    else
        return -1;
}

bytes dev::fromHex(std::string const& _s, WhenError _throw)
{
    unsigned s = (_s[0] == '0' && _s[1] == 'x') ? 2 : 0;
    std::vector<uint8_t> ret;
    ret.reserve((_s.size() - s + 1) / 2);

    if (_s.size() % 2)
    {
        int h = fromHex(_s[s++], WhenError::DontThrow);
        if (h != -1)
            ret.push_back(h);
        else if (_throw == WhenError::Throw)
            BOOST_THROW_EXCEPTION(BadHexCharacter());
        else
            return bytes();
    }
    for (unsigned i = s; i < _s.size(); i += 2)
    {
        int h = fromHex(_s[i], WhenError::DontThrow);
        int l = fromHex(_s[i + 1], WhenError::DontThrow);
        if (h != -1 && l != -1)
            ret.push_back((uint8_t)(h * 16 + l));
        else if (_throw == WhenError::Throw)
            BOOST_THROW_EXCEPTION(BadHexCharacter());
        else
            return bytes();
    }
    return ret;
}

bool dev::setenv(const char name[], const char value[], bool override)
{
#if _WIN32
    if (!override && std::getenv(name) != nullptr)
        return true;

    return ::_putenv_s(name, value) == 0;
#else
    return ::setenv(name, value, override ? 1 : 0) == 0;
#endif
}

std::string dev::getTargetFromDiff(double diff, HexPrefix _prefix)
{
    using BigInteger = boost::multiprecision::cpp_int;

    // Stratum difficulty is expressed in units of 2^32 hashes (difficulty 1 == 2^32 hashes),
    // the same convention pools use (see miningcore-xhash EthereumJob). Parallax XHash defines
    // target = floor((2^256 - 1) / hashes), so target = floor((2^256 - 1) / (diff * 2^32)).
    // Integer arithmetic only: scale the double by 2^32 exactly and divide.
    static const BigInteger maxTarget = (BigInteger(1) << 256) - 1;

    BigInteger product = maxTarget;
    if (std::isfinite(diff) && diff > 0)
    {
        BigInteger hashes(std::ldexp(diff, 32));  // truncates toward zero
        if (hashes > 1)
            product = maxTarget / hashes;
    }

    // Normalize to 64 chars hex with "0x" prefix
    stringstream ss;
    ss << (_prefix == HexPrefix::Add ? "0x" : "") << setw(64) << setfill('0') << std::hex
       << product;

    string target = ss.str();
    boost::algorithm::to_lower(target);
    return target;
}

double dev::getHashesToTarget(string _target)
{
    using BigInteger = boost::multiprecision::cpp_int;

    // Expected number of hashes to find a solution below _target: (2^256 - 1) / target
    static const BigInteger maxTarget = (BigInteger(1) << 256) - 1;
    BigInteger divisor(_target);
    if (divisor == 0)
        return 0.0;
    return (maxTarget / divisor).convert_to<double>();
}

std::string dev::getScaledSize(double _value, double _divisor, int _precision, string _sizes[],
    size_t _numsizes, ScaleSuffix _suffix)
{
    double _newvalue = _value;
    size_t i = 0;
    while (_newvalue > _divisor && i < (_numsizes - 1))
    {
        _newvalue /= _divisor;
        i++;
    }

    std::stringstream _ret;
    _ret << fixed << setprecision(_precision) << _newvalue;
    if (_suffix == ScaleSuffix::Add)
        _ret << " " << _sizes[i];
    return _ret.str();
}

std::string dev::getFormattedHashes(double _hr, ScaleSuffix _suffix, int _precision)
{
    static string suffixes[] = {"h", "Kh", "Mh", "Gh", "Th"};
    return dev::getScaledSize(_hr, 1000.0, _precision, suffixes, 5, _suffix);
}

std::string dev::getFormattedMemory(double _mem, ScaleSuffix _suffix, int _precision)
{
    static string suffixes[] = {"B", "KB", "MB", "GB"};
    return dev::getScaledSize(_mem, 1024.0, _precision, suffixes, 4, _suffix);
}

std::string dev::padLeft(std::string _value, size_t _length, char _fillChar) 
{
    if (_length > _value.size())
        _value.insert(0, (_length - _value.size()), _fillChar);
    return _value;
}

std::string dev::padRight(std::string _value, size_t _length, char _fillChar)
{
    if (_length > _value.size())
        _value.resize(_length, _fillChar);
    return _value;
}
