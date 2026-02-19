mkdir -p ~/local/{include,lib,bin}
export PREFIX=~/local
export PATH=$PREFIX/bin:$PATH
export LD_LIBRARY_PATH=$PREFIX/lib:$LD_LIBRARY_PATH
export CMAKE_PREFIX_PATH=$PREFIX:$CMAKE_PREFIX_PATH

# Eigen3 (header-only)
cd /tmp && wget https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
tar xzf eigen-3.4.0.tar.gz
cp -r eigen-3.4.0/Eigen $PREFIX/include/
cp -r /tmp/eigen-3.4.0/unsupported $HOME/local/include/
export EIGEN3_INCLUDE_DIR=$PREFIX/include

# Spectra (header-only)
cd /tmp && wget https://github.com/yixuan/spectra/archive/refs/tags/v1.0.1.tar.gz
tar xzf v1.0.1.tar.gz
cp -r spectra-1.0.1/include/Spectra $PREFIX/include/
export SPECTRA_LIB=$PREFIX/include

# Boost (header-only — GCTA only uses header-only parts)
# If your system already has boost headers in /usr/include, just point to that
export BOOST_LIB=/usr/include

# zlib
cd /tmp && wget https://zlib.net/zlib-1.3.1.tar.gz
tar xzf zlib-1.3.1.tar.gz && cd zlib-1.3.1
./configure --prefix=$PREFIX --static
make -j$(nproc) && make install

# zstd
cd /tmp && wget https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
tar xzf zstd-1.5.6.tar.gz && cd zstd-1.5.6
make -j$(nproc) PREFIX=$PREFIX lib-install

# SQLite3
cd /tmp && wget https://www.sqlite.org/2024/sqlite-autoconf-3450000.tar.gz
tar xzf sqlite-autoconf-3450000.tar.gz && cd sqlite-autoconf-3450000
./configure --prefix=$PREFIX --enable-static --disable-shared
make -j$(nproc) && make install

# GSL
cd /tmp && wget https://ftp.gnu.org/gnu/gsl/gsl-2.8.tar.gz
tar xzf gsl-2.8.tar.gz && cd gsl-2.8
./configure --prefix=$PREFIX --enable-static --disable-shared
make -j$(nproc) && make install

conda install -c conda-forge mkl mkl-devel
export MKLROOT=$CONDA_PREFIX
