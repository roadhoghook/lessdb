//
// Created by neverchanje on 8/24/15.
//

#ifndef LESSDB_LESSDB_H
#define LESSDB_LESSDB_H

#include "dbImpl.h"
#include "status.h"
#include <string>
#include <boost/noncopyable.hpp>

class Collection;

class LessDB : boost::noncopyable
{

public:

    LessDB() {};

    ~LessDB() {};

    Status command(const std::string& line);

private:

    dbImpl impl;

};

#endif //LESSDB_LESSDB_H
