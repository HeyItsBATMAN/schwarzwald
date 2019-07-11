FROM conanio/gcc7

RUN whoami

RUN pip install conan --upgrade
RUN conan remote add bincrafters https://api.bintray.com/conan/bincrafters/public-conan
#RUN apt-get update && apt-get install -y git cmake libboost-all-dev

RUN sudo mkdir /data
RUN sudo chown -c conan /data 
WORKDIR /data

RUN git clone https://github.com/m-schuetz/LAStools.git
WORKDIR /data/LAStools/LASzip
RUN mkdir build
RUN cd build && cmake -DCMAKE_BUILD_TYPE=Release ..
RUN cd build && make

WORKDIR /data

# Check out and build terminalpp. Checking out a specific commit prior to the change to using fmt library because it does not link with fmt. Fixing this is a TODO
RUN git clone https://github.com/KazDragon/terminalpp.git && cd ./terminalpp && git checkout 68b497734c57239061b36aee0abf947db0f51c9f

#TODO Set C++ library version to libstdc++11
#RUN sudo mkdir ~/.conan/profiles
#RUN sudo sh -c 'echo "compiler.libcxx=libstdc++11" > ~/.conan/profiles/default'

WORKDIR /data/terminalpp

RUN conan install .
RUN cmake -DCMAKE_BUILD_TYPE=Release .
RUN make

WORKDIR /data
RUN mkdir PotreeConverter
WORKDIR /data/PotreeConverter
ADD . /data/PotreeConverter
RUN mkdir build
RUN cd build && cmake -DCMAKE_BUILD_TYPE=Release -DLASZIP_INCLUDE_DIRS=/data/LAStools/LASzip/dll -DLASZIP_LIBRARY=/data/LAStools/LASzip/build/src/liblaszip.so -DTERMINALPP_INCLUDE_DIRS=/data/terminalpp/include -DTERMINALPP_LIBRARY=/data/terminalpp/libterminalpp.a .. 
RUN cd build && make
RUN cp -R /data/PotreeConverter/PotreeConverter/resources/ /data

ENTRYPOINT ["/data/PotreeConverter/build/Release/PotreeConverter"]
