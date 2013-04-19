#! /bin/bash
# ./test_rewrite_linux.sh <rewrite> <param> <predicted cache size> 

echo $#
if [ $# -lt 3 ]; then
echo "invalid parameters"
exit
fi

./rebuild

dataset=/home/fumin/linux_txt

rewrite="--rewrite=$1"

if [ $1 = "CFL" ]; then
param="--cfl_p=$2"
elif [ $1 = "CBR" ]; then
param="--rewrite_limit=$2"
elif [ $1 = "CAP" ]; then
param="--capping_t=$2"
fi

if [ $3 -gt 0 ]; then
pcache="--cache_size=$3 --enable_cache_filter"
else
pcache=""
fi

echo "rewrite=$1 param=$2 pcache=$3" >>  backup.log
jobid=0
for file in $(ls $dataset); do
destor $dataset/$file --index=RAM $rewrite $param $pcache >>log;
jobid=$(($jobid+1));
done

jobid=$(($jobid-1));
destor -s >> backup.log;

echo "rewrite=$1 param=$2 pcache=$3" >> restore.log
echo "lru_cache=20" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=20 --enable_simulator >>log;
i=$(($i+1));
done

echo "rewrite=$1 param=$2 pcache=$3" >> restore.log
echo "lru_cache=12" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=12 --enable_simulator >>log;
i=$(($i+1));
done

echo "lru_cache=10" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=10 --enable_simulator >>log;
i=$(($i+1));
done

echo "lru_cache=8" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=8 --enable_simulator >>log;
i=$(($i+1));
done

echo "lru_cache=6" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=6 --enable_simulator >>log;
i=$(($i+1));
done

echo "#lru_cache=4" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=4 --enable_simulator >>log;
i=$(($i+1));
done

echo "lru_cache=2" >> restore.log
i=0
while [ $i -le $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=LRU --cache_size=2 --enable_simulator >>log;
i=$(($i+1));
done
