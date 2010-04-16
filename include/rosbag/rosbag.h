/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

#ifndef ROSBAG_H
#define ROSBAG_H

#include "ros/header.h"
#include "ros/time.h"
#include "ros/message.h"
#include "ros/message_traits.h"
#include "ros/ros.h"

#include "rosbag/constants.h"

#include <boost/foreach.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iterator/iterator_facade.hpp>

#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <set>
#include <algorithm>
#include <queue>
#include <string>

namespace rosbag
{
  typedef unsigned long long pos_t;

  namespace bagmode
  {
    //! Enumerated modes
    enum BagMode
      {
        Read   = 0x01, //!< Open a bag file for reading
        Write  = 0x02, //!< Open a bag file for writing
        Append = 0x04, //!< Open a bag file for appending
        Default   = Read //!< Enable "intensities" and "stamps" channels
      };
  }

  //! Struct to store message information
  struct MsgInfo
  {
    std::string topic;
    std::string msg_def;
    std::string md5sum;
    std::string datatype;
  };
  
  //! Struct to store index information
  struct IndexEntry
  {
    ros::Time time;
    pos_t    pos;
  };

  //! Comparator to sort MessageInstances by time-stamp
  struct IndexEntryCompare
  {
    bool operator() (const ros::Time& a, const IndexEntry& b) const
    {
      return a < b.time;
    }
    bool operator() (const IndexEntry& a, const ros::Time& b) const
    {
      return a.time < b;
    }
  };

  class MessageInstance;
  class MessageInstanceCompare;


  //! Typedef for index: map of topic_name -> list of MessageInstance
  typedef std::vector<MessageInstance> MessageList;

  //! Intended typedef to use for the Bag Index.
  typedef std::map<std::string, MessageList> BagIndex;

  //! Base class for rosbag exceptions
  class Exception : public std::runtime_error {};

  //! Exception thrown if trying to add or read from an unopened recorder/player
  class BagNotOpenException : public Exception {};

  //! Exception thrown when assorted IO problems
  class BagIOException : public Exception {};

  //! Exception thrown if an invalid MsgPos (such as from another bag)
  //  is passed to seek
  class InvalidMsgPosException : public Exception {};

  //! Exception thrown if trying to instantiate a MsgInstance as the
  //  wrong type
  class InstantiateException : public Exception {};

  class View;
  class Query;

  //! Class for writinging to a bagfile
  class Bag
  {
    friend class MessageInstance;
    friend class View;

  public:
     //! Constructor
    Bag();

    //! Destructor
    ~Bag();
    
    //! Open a bagfile by name 
    bool open(const std::string &file_name, int mode = bagmode::Default);
    
    //! Close bagfile 
    /*!
     *  Make sure bagfile is written out to disk, index is appended,
     *  file descriptor is closed, etc..
     */
    void close();
    
    //! Write a message into the bag file
    /*!
     * \param topic_name  The topic name
     * \param time        Timestamp of the message
     * \param msg         A pointer to the message to be added.
     *
     * Can throw BagNotOpenException or BagIOException
     */
    void write(const std::string& topic_name, ros::Time time, ros::Message::ConstPtr msg);

    //! Write a message into the bag file
    /*!
     * \param topic_name  The topic name
     * \param time        Timestamp of the message
     * \param msg         A const reference to a message
     *
     * Can throw BagNotOpenException or BagIOException
     */
    void write(const std::string& topic_name, ros::Time time, const ros::Message& msg);

    //! Write a message into the bag file
    /*!
     * \param topic_name  The topic name
     * \param time        Timestamp of the message
     * \param msg         A MessageInstance as reterned by reader
     *
     * Can throw BagNotOpenException or BagIOException
     */
    void write(const std::string& topic_name, ros::Time time, const MessageInstance& msg);

  protected:
    //! Actually try to load a record from a particular offset

    // NOTE: THIS SHOULD PROBABLY TAKE A MESSAGEINSTANCE

    template <class T>
    boost::shared_ptr<T const> instantiate(uint64_t pos)
    {

      boost::shared_ptr<T> p;

      read_stream_.seekg(pos, std::ios::beg);

      ros::Header header;
      uint32_t data_size;
      ros::M_string::const_iterator fitr;      
      ros::M_stringPtr fields_ptr;
      unsigned char op;

      // We skip and read the following message if it's a DEF
      do
      {
        if (!readHeader(header, data_size))
          return p;
      
        fields_ptr = header.getValues();
        ros::M_string& fields = *fields_ptr;

        if((fitr = checkField(fields, OP_FIELD_NAME,
                              1, 1, true)) == fields.end())
          return p;
      
        memcpy(&op,fitr->second.data(),1);
      
      // If it's a DEF, read the immediately following message (since def has no body, no seeking necessary)
      } while (op == OP_MSG_DEF);

      assert (op == OP_MSG_DATA);

      ros::M_string& fields = *fields_ptr;

      std::string topic_name;
      std::string md5sum;
      std::string datatype;
      std::string message_definition;
      
      if((fitr = checkField(fields, TOPIC_FIELD_NAME,
                            1, UINT_MAX, true)) == fields.end())
        return p;
      topic_name = fitr->second;
      
      if((fitr = checkField(fields, MD5_FIELD_NAME,
                            32, 32, true)) == fields.end())
        return p;
      md5sum = fitr->second;

      // Make sure it all checks out.
      assert(ros::message_traits::MD5Sum<T>::value() == md5sum ||
        ros::message_traits::MD5Sum<T>::value()[0] == '*');
      
      if((fitr = checkField(fields, TYPE_FIELD_NAME,
                            1, UINT_MAX, true)) == fields.end())
        return p;
      datatype = fitr->second;      
      
      message_buf_len_ = data_size;

      if (message_buf_len_ > message_buf_size_)
      {
        if (message_buf_)
          delete[] message_buf_;
        message_buf_size_ = message_buf_len_ * 2;
        message_buf_ = new uint8_t[message_buf_size_];
      }

      // Read in the message body
      read_stream_.read((char*)message_buf_, message_buf_len_);

      boost::shared_ptr<ros::M_string> connection_header(new ros::M_string);
      (*connection_header)["md5sum"] = md5sum;
      (*connection_header)["type"] = datatype;
      // TODO: Maybe set message_definition from bag. No idea how this
      // could be done.
      (*connection_header)["message_definition"] = message_definition;
      p = boost::shared_ptr<T>(new T());
      p->__connection_header = connection_header;
      p->__serialized_length = message_buf_len_;
      p->deserialize(message_buf_);

      return p;

    }

  private:

    bool readMode()   { return mode_ & bagmode::Read; }
    bool writeMode()  { return mode_ & bagmode::Write; }
    bool appendMode() { return mode_ & bagmode::Append; }

    void         closeFile();

    const void*  getHeaderBuffer();
    unsigned int getHeaderBufferLength();
    void         resetHeaderBuffer();
    void         writeFieldToHeaderBuffer(const std::string& name, const void* value, unsigned int value_len);
    bool         checkDisk();
    
    void         writeVersion();
    void         writeFileHeader();
    void         writeIndex();
    
    void         writeRecord(const ros::M_string& fields, const char* data, uint32_t data_len);
    void         writeHeader(const ros::M_string& fields, uint32_t data_len);
    void         writefil(const char* s, std::streamsize n);
    void         writefil(const std::string& s);
    void         seek(pos_t pos);


    // Player helper variables
    int version_;
    int version_major_;
    int version_minor_;


    // Player functions
    bool readHeader(ros::Header& header, uint32_t& next_msg_size);
    bool readVersion();
    bool readFileHeader();
    bool readIndex();
    bool readDefs();
    bool readDef(uint64_t);
    ros::M_string::const_iterator checkField(const ros::M_string& fields,
                                             const std::string& field,
                                             unsigned int min_len,
                                             unsigned int max_len,
                                             bool required);

    // Other stufff... oh how this needs brutal cleaning

    int mode_;

    std::string file_name_;
    
    std::ifstream                       read_stream_;
    std::ofstream                       write_stream_;

    pos_t                               record_pos_;
    pos_t                               file_header_pos_;
    pos_t                               index_data_pos_;

    boost::mutex                        record_mutex_;
    
    // Keep track of which topics have had their message definitions written already
    std::map<std::string, MsgInfo>      topics_recorded_;
    boost::mutex                        topics_recorded_mutex_;
    
    std::map<std::string, std::vector<IndexEntry> > topic_indexes_;
    
    // Reusable buffer in which to assemble the message header before writing to file
    unsigned char* header_buf_;
    unsigned int   header_buf_len_;
    unsigned int   header_buf_size_;
    
    // Reusable buffer in which to assemble the message data before writing to file
    unsigned char* message_buf_;
    unsigned int   message_buf_len_;
    unsigned int   message_buf_size_;
    
    bool writing_enabled_;
    
    boost::mutex       check_disk_mutex_;
    ros::WallTime      check_disk_next_;
    ros::WallTime      warn_next_;

  };


  // A base class for specifying queries from a bag
  class Query
  {
  public:
    Query(const ros::Time& begin_time = ros::TIME_MIN, const ros::Time& end_time = ros::TIME_MAX ) :
      begin_time_(begin_time),
      end_time_(end_time) { }

    ros::Time getBeginTime() const { return begin_time_; }

    ros::Time getEndTime() const { return end_time_; }

    virtual bool evaluate(const MsgInfo& info) const { return true; }

  private:
    ros::Time begin_time_;
    ros::Time end_time_;
  };

  // Subclass of query to get messages on a specified topic
  class TopicQuery : public Query
  {
  public:
    TopicQuery(const std::vector<std::string> topics, const ros::Time& begin_time = ros::TIME_MIN, const ros::Time& end_time = ros::TIME_MAX ) :
      Query(begin_time, end_time),
      topics_(topics) { }

    virtual bool evaluate(const MsgInfo& info) const {
      BOOST_FOREACH( std::string t, topics_ )
      {
        if (t == info.topic)
          return true;
      }
      return false;
    }

  private:
    std::vector<std::string> topics_;
  };

  // Pairs of queries and the bags they come from (Internally used by View)
  typedef std::pair<Bag*, Query> BagQuery;

  // A Range of messages from a particular bag query -- used for iteration logic
  struct MessageRange
  {
    std::vector<IndexEntry>::const_iterator begin;
    std::vector<IndexEntry>::const_iterator end;
   
    // ***********************************
    // **************WARNING**************
    // ***********************************
    // Double check if this is safe -- this is a pointer to a data
    // structure stored in a map.
    const MsgInfo* msg_info;

    // Pointer to vector of queries in View
    const BagQuery* bag_query;
    
    MessageRange(const std::vector<IndexEntry>::const_iterator& _begin,
                const std::vector<IndexEntry>::const_iterator& _end,
                const MsgInfo* _msg_info,
                const BagQuery* _bag_query) : begin(_begin), end(_end), msg_info(_msg_info), bag_query(_bag_query) {}
  };

  // The actual iterator data structure
  struct ViewIterHelper
  {
    std::vector<IndexEntry>::const_iterator iter;

    // Pointer to vector of ranges in View
    const MessageRange* range;

    ViewIterHelper(std::vector<IndexEntry>::const_iterator _iter, const MessageRange* _range) :
      iter(_iter), range(_range) { }
  };

  // Comparison that allows us to easily sort of ViewIterHelpers
  struct ViewIterHelperCompare
  {
    bool operator() (const ViewIterHelper& a, const ViewIterHelper& b) const
    {
      return (a.iter)->time > (b.iter)->time;
    }
  };

  // Our current View has a bug.  Our internal storage end is based on
  // an iterator element rather than a time.  This means if we update
  // the underlying index to include a new message after our end-time
  // but before where our end-iterator points, our view will include
  // it when it shouldn't.  Similarly, adding new messages before our
  // first message but after our start time won't be capture in the
  // view either.
  class View
  {
    friend class Bag;
  public:
    // Our iterator still internally stores a MessageList::const_iterator which constrains its ability to dereference

    // Making this reverse-traversable is going to be trickier...
    class iterator : public boost::iterator_facade<iterator, MessageInstance const, boost::forward_traversal_tag>
    {
    public:
      iterator(iterator const& other) : view_(other.view_), iters_(other.iters_) {}

    protected:
      // Note: the default constructor on the merge_queue means this is an empty queue -- our definition of end.
      iterator(const View* view, const std::vector<ViewIterHelper>& iters) : view_(view), iters_(iters) {}

    private:
      friend class View;
      friend class boost::iterator_core_access;

      bool equal(iterator const& other) const;

      void increment();

      // Leaving this const for now, even though it doesn't have to be
      const MessageInstance dereference() const;

      const View* view_;
      std::vector<ViewIterHelper> iters_;

    };

    typedef iterator const_iterator;

    iterator begin() const;
    iterator end()   const;
    uint32_t size()  const;

    View() {}

    ~View() {
      BOOST_FOREACH(MessageRange* mr, ranges_)
      {
        delete mr;
      }
      BOOST_FOREACH(BagQuery* bq, queries_)
      {
        delete bq;
      }
    }

    
    void addQuery(Bag& bag, const Query& query);

  private:

    std::vector<MessageRange*> ranges_;
    std::vector<BagQuery*> queries_;

  };


  //! Class representing an actual message
  class MessageInstance
  {
    friend class Bag;
    friend class View::iterator;

  public:
    const std::string getTopic() const
    {
      return info_->topic;
    }
    const std::string getDatatype() const
    {
      return info_->datatype;
    }
    const std::string getMd5sum() const
    {
      return info_->md5sum;
    }
    const std::string getDef() const
    {
      return info_->msg_def;
    }
    const ros::Time getTime() const
    {
      return index_->time;
    };

    /*!
     * Templated type-check 
     */
    template <class T>  bool isType() const
    {
      return (ros::message_traits::MD5Sum<T>::value() == getMd5sum() &&
              ros::message_traits::DataType<T>::value() == getDatatype());
    }

    /*!
     * Templated instantiate
     */
    template <class T>
    boost::shared_ptr<T const> instantiate() const
    {

      if (ros::message_traits::MD5Sum<T>::value() != getMd5sum() &&
        ros::message_traits::MD5Sum<T>::value()[0] != '*')
        return boost::shared_ptr<T const>();
      {
        return bag_->instantiate<T>(index_->pos);
      }
    }

  protected:
    MessageInstance(const MsgInfo& info,
                    const IndexEntry& ind,
                    Bag& bag) : info_(&info), index_(&ind), bag_(&bag) {}
    
  private:
    const MsgInfo*    info_;
    const IndexEntry* index_;
    Bag*              bag_;
  };

  //! Comparator to sort MessageInstances by time-stamp
  struct MessageInstanceCompare
  {
    bool operator() (const MessageInstance& a, const MessageInstance& b) const
    {
      if (a.getTime() < b.getTime()) 
        return true;
      else
        return false;
    }
  };

}

#endif
