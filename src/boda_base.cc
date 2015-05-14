// Copyright (c) 2013-2014, Matthew W. Moskewicz <moskewcz@alumni.princeton.edu>; part of Boda framework; see LICENSE
#include"boda_tu_base.H"
#include"str_util.H"
#include"pyif.H"
#include<memory>
#include<execinfo.h>
#include<cxxabi.h>
#include<boost/filesystem.hpp>
#include<boost/iostreams/device/mapped_file.hpp>
#include<sys/mman.h>
#include<sys/stat.h>
#include<fcntl.h>

namespace boda 
{
  using std::get_deleter;
  using std::unique_ptr;
  using std::ifstream;
  using std::ofstream;
  using boost::filesystem::path;
  using boost::filesystem::filesystem_error;

  // prints errno
  void neg_one_fail( int const & ret, char const * const func_name ) {
    if( ret == -1 ) { rt_err( strprintf( "%s() failed with errno=%s (%s)", func_name, str(errno).c_str(),
					 strerror(errno) ) ); }
  }

  void rt_err_errno( char const * const func_name ) { rt_err( strprintf( "%s failed with errno=%s (%s)", func_name, str(errno).c_str(),
									 strerror(errno) ) ); }

  template< typename T >
  string ssds_str( T const & o1, T const & o2 ) {
    double ssds = 0, sds = 0;
    sum_squared_diffs( ssds, sds, o1->elems, o2->elems );
    double const aad = sqrt(ssds / o1->elems.sz);
    double const ad = sds / o1->elems.sz;
    return strprintf( "cnt=%s sum_squared_diffs=%s avg_abs_diff=%s sum_diffs=%s avg_diff=%s", 
		      str( o1->elems.cnt_diff_elems( o2->elems ) ).c_str(),
		      str( ssds ).c_str(), str( aad ).c_str(),
		      str( sds ).c_str(), str( ad ).c_str() );
  }

  template string ssds_str< p_nda_float_t >( p_nda_float_t const & o1, p_nda_float_t const & o2 );
  template string ssds_str< p_nda_double_t >( p_nda_double_t const & o1, p_nda_double_t const & o2 );
  

  // questionably, we use (abuse?) the fact that we can mutate the
  // deleter to support mremap()ing the memory pointed to by the
  // deleter's (unique) shared_ptr.
  struct uint8_t_munmap_deleter { 
    size_t length;
    uint8_t_munmap_deleter( size_t const & length_ ) : length(length_) { assert(length); }
    void operator()( uint8_t * const & b ) const { if( !length ) { return; } if( munmap( b, length) == -1 ) { rt_err("munmap"); } } 
  };

  p_uint8_t make_mmap_shared_p_uint8_t( int const fd, size_t const length, off_t const offset ) {
    int flags = MAP_SHARED;
    if( fd == -1 ) { assert_st( !offset ); flags |= MAP_ANONYMOUS; }
    void * ret = mmap( 0, length, PROT_READ | PROT_WRITE, flags, fd, offset);
    if( MAP_FAILED == ret ) { rt_err_errno("mmap(...)"); }
    return p_uint8_t( (uint8_t *)ret, uint8_t_munmap_deleter( length ) ); 
  }

  void remap_mmap_shared_p_uint8_t( p_uint8_t &p, size_t const new_length ) {
    assert_st( p.unique() );
    uint8_t_munmap_deleter * d = get_deleter<uint8_t_munmap_deleter>(p);
    assert_st( d );
    assert( new_length > d->length );
    void * new_d = mremap( p.get(), d->length, new_length, MREMAP_MAYMOVE );
    if( MAP_FAILED == new_d ) { rt_err_errno("mmap(...)"); }
    d->length = 0; // make currently deleter into a no-op
    p.reset( (uint8_t *)new_d, uint8_t_munmap_deleter( new_length ) );
  }

  void * posix_memalign_check( size_t const sz, uint32_t const a ) {
    void * p = 0;
    int const ret = posix_memalign( &p, a, sz );
    if( ret ) { rt_err( strprintf( "posix_memalign( p, %s, %s ) failed, ret=%s", 
				   str(a).c_str(), str(sz).c_str(), str(ret).c_str() ) ); }
    return p;
  }

  bool ensure_is_dir( string const & fn, bool const create ) { 
    path const p(fn);
    try  { 
      bool const is_dir_ret = is_directory( p );
      if( (!create) && (!is_dir_ret) ) { 
	rt_err( strprintf("expected path '%s' to be a directory, but it is not.", p.c_str() ) ); 
      } 
      if( is_dir_ret ) { return 0; }
    } catch( filesystem_error const & e ) {
      rt_err( strprintf( "filesystem error while trying to check if '%s' is a directory: %s", 
			 p.c_str(), e.what() ) ); 
    }
    try  { // if we get here, p is not a directory, so try to create it
      bool const cd_ret = boost::filesystem::create_directory( p );
      assert_st( cd_ret == 1 ); // should not already be dir, so we should either create or raise an expection
      return 1;
    } catch( filesystem_error const & e ) {
      rt_err( strprintf( "error while trying to create '%s' directory: %s", 
			 p.c_str(), e.what() ) ); 
    }
  }
  void ensure_is_regular_file( string const & fn ) { 
    path const p( fn );
    try  { 
      bool const ret = is_regular_file( p ); 
      if( !ret ) { rt_err( strprintf("expected path '%s' to be a regular file, but it is not.", p.c_str()));}}
    catch( filesystem_error const & e ) {
      rt_err( strprintf( "filesystem error while trying to check if '%s' is a regular file: %s", 
			 p.c_str(), e.what() ) ); }
  }

  void set_fd_cloexec( int const fd, bool const val ) {
    int fd_flags = 0;
    neg_one_fail( fd_flags = fcntl( fd, F_GETFD ), "fcntl" );
    if( val ) { fd_flags |= FD_CLOEXEC; }
    else { fd_flags &= ~FD_CLOEXEC; }
    neg_one_fail( fcntl( fd, F_SETFD, fd_flags ), "fcntl" );
  }

  vect_rp_const_char get_vect_rp_const_char( vect_string const & v ) {
    vect_rp_const_char ret;
    for( vect_string::const_iterator i = v.begin(); i != v.end(); ++i ) { ret.push_back( (char *)i->c_str() ); }
    return ret;
  }
  vect_rp_char get_vect_rp_char( vect_string const & v ) {
    vect_rp_char ret;
    for( vect_string::const_iterator i = v.begin(); i != v.end(); ++i ) { ret.push_back( (char *)i->c_str() ); }
    return ret;
  }


  void fork_and_exec_self( vect_string const & args ) {
    vect_rp_char argp = get_vect_rp_char( args );
    argp.push_back( 0 );
    string const self_exe = py_boda_dir() + "/lib/boda"; // note: uses readlink on /proc/self/exe internally
    pid_t const ret = fork();
    if( ret == 0 ) {
      execve( self_exe.c_str(), &argp[0], environ );
      rt_err( strprintf( "execve of '%s' failed. envp=environ, args=%s", self_exe.c_str(), str(args).c_str() ) );
    }
    // ret == child pid, not used
  }

  // opens a ifstream. note: this function itself will raise if the open() fails.
  p_ifstream ifs_open( filename_t const & fn )
  {
    ensure_is_regular_file( fn.exp );
    p_ifstream ret( new ifstream );
    ret->open( fn.exp.c_str() );
    if( ret->fail() ) { rt_err( strprintf( "can't open file '%s' for reading", fn.in.c_str() ) ); }
    assert( ret->good() );
    return ret;
  }
  p_ifstream ifs_open( std::string const & fn ) { return ifs_open( filename_t{fn,fn} ); }

  // clears line and reads one line from in. returns true if at EOF. 
  // note: calls rt_err() if a complete line cannot be read.
  bool ifs_getline( std::string const &fn, p_ifstream in, string & line )
  {
    line.clear();
    // the file should initially be good (including if we just
    // opened it).  note the eof is not set until trying to read
    // past the end. after each line is read, we check for eof, and
    // if we're not at eof, we check that the stream is still good
    // for more reading.
    assert_st( in->good() ); 
    getline(*in, line);
    if( in->eof() ) { 
      if( !line.empty() ) { rt_err( "reading "+fn+": incomplete (no newline) line at EOF:'" + line + "'" ); } 
      return 1;
    }
    else {
      if( !in->good() ) { rt_err( "reading "+fn+ " unknown failure" ); }
      return 0;
    }
  }


  p_vect_string readlines_fn( filename_t const & fn ) {
    p_ifstream in = ifs_open( fn );
    p_vect_string ret( new vect_string );
    string line;
    while( !ifs_getline( fn.in, in, line ) ) { ret->push_back( line ); }
    return ret;
  }
  p_vect_string readlines_fn( string const & fn ) { return readlines_fn( filename_t{fn,fn} ); }

  // opens a ofstream. note: this function itself will raise if the open() fails.
  p_ofstream ofs_open( filename_t const & fn )
  {
    p_ofstream ret( new ofstream );
    ret->open( fn.exp.c_str() );
    if( ret->fail() ) { rt_err( strprintf( "can't open file '%s' for writing", fn.in.c_str() ) ); }
    assert( ret->good() );
    return ret;
  }
  p_ofstream ofs_open( std::string const & fn ) { return ofs_open( filename_t{fn,fn} ); }

  p_mapped_file_source map_file_ro( filename_t const & fn ) {
    //ensure_is_regular_file( fn ); // too strong? a good idea?
    p_mapped_file_source ret;
    try { ret.reset( new mapped_file_source( fn.exp ) ); }
    catch( std::exception & err ) { 
      // note: fn.c_str(),err.what() is not too useful? it does give 'permission denied' sometimes, but other times is it just 'std::exception'.
      rt_err( strprintf("failed to open/map file '%s' for reading",fn.in.c_str()) ); 
    }
    assert_st( ret->is_open() ); // possible?
    return ret;
  }
  p_mapped_file_source map_file_ro( std::string const & fn ) { return map_file_ro( filename_t{fn,fn} ); }

  p_string read_whole_fn( filename_t const & fn ) {
    p_mapped_file_source mfile = map_file_ro( fn );
    uint8_t const * const fn_data = (uint8_t const *)mfile->data();
    return p_string( new string( fn_data, fn_data+mfile->size() ) );
  }
  void write_whole_fn( filename_t const & fn, std::string const & data ) {
    p_ofstream out = ofs_open( fn );
    (*out) << data;
  }

  uint32_t const max_frames = 64;

  p_vect_rp_void get_backtrace( void )
  {
    // we could easily double bt until the trace fits, but for now
    // we'll assume something has gone wrong if the trace is >
    // max_frames and allow truncation. this also (hopefully) limits
    // the cost of backtrace().
    p_vect_rp_void bt( new vect_rp_void );
    bt->resize( max_frames );
    int const bt_ret = backtrace( &bt->at(0), bt->size() );
    assert( bt_ret >= 0 );
    assert( uint32_t(bt_ret) <= bt->size() );
    bt->resize( bt_ret );
    return bt;
  }

  string stacktrace_str( p_vect_rp_void bt, uint32_t strip_frames )
  {
    string ret;
    assert( !bt->empty() );
    ret += strprintf( "----STACK TRACE (FRAMES=%s-%s)%s----\n", 
		      str(bt->size()).c_str(), str(strip_frames).c_str(), 
		      (bt->size() < max_frames)?"":" <WARNING: frames may be truncated (oldest lost)>" );
    unique_ptr< char * > bt_syms( backtrace_symbols( &bt->at(0), bt->size() ) );
    for( uint32_t i = strip_frames; i < bt->size(); ++i )
    {
      // it's a little unclear what should be assert()s here versus possible/ignorable/handlable cases
      // note: we can't use assert_st() here
      string sym( bt_syms.get()[i] );
      size_t const op_pos = sym.find('(');
      assert( op_pos != string::npos ); // can't find '(' to start symbol name
      assert( (op_pos+1) < sym.size() ); // '(' should not be last char. 
      sym[op_pos] = 0; // terminate object/file name part
      size_t const plus_pos = sym.find('+',op_pos);
      if( plus_pos != string::npos ) { // if there was an '+' (i.e. an offset part)
	assert( (plus_pos+1) < sym.size() ); // '+' should not be last char. 
	sym[plus_pos] = 0; // terminate sym_name part
      }
      size_t const cp_pos = sym.find(')',(plus_pos!=string::npos)?plus_pos:op_pos);
      assert( cp_pos != string::npos ); // can't find ')' to end symbol(+ maybe offset) part      
      assert( (cp_pos+1) < sym.size() ); // ')' should not be last char. 
      sym[cp_pos] = 0; // terminate sym_name (+ maybe offset) part

      char * sym_name = &sym[ op_pos + 1 ];
      int dm_ret = 0;
      unique_ptr< char > dm_fn( abi::__cxa_demangle(sym_name, 0, 0, &dm_ret ) );
      if( dm_ret == 0 ) { sym_name = dm_fn.get(); }
      ret += strprintf( "  %s(%s%s%s)%s\n", sym.c_str(), sym_name, 
			(plus_pos==string::npos)?"":"+",
			(plus_pos==string::npos)?"":(sym.c_str()+plus_pos+1),
			sym.c_str()+cp_pos+1 ); 
    }
    return ret;
  }

  rt_exception::rt_exception( std::string const & err_msg_, p_vect_rp_void bt_ ) : err_msg(err_msg_), bt(bt_) {}
  char const * rt_exception::what( void ) const throw() { return err_msg.c_str(); }
  string rt_exception::what_and_stacktrace( void ) const { return err_msg + "\n" + stacktrace_str( bt, 2 ); }
  int rt_exception::get_ret_code( void ) const { return 1; }
  void rt_err( std::string const & err_msg ) { throw rt_exception( "error: " + err_msg, get_backtrace() ); }

}
