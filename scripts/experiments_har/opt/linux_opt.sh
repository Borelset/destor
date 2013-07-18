#! /bin/sh

index=RAM
rewrite=HBR
dataset=/home/dataset/linux_src_trace

./rebuild
echo "index ${index} rewrite $rewrite" >>  backup.log
jobid=0
for file in $(ls $dataset); do
destor $dataset/$file --index=$index --rewrite=$rewrite >>log
if [ $jobid -ge 20 ]; then
destor -d$(($jobid-20)) --rewrite=$rewrite --kept_versions=20
fi
jobid=$(($jobid+1))
done

destor -s >> backup.log

for window_size in 40000 20000 10000 5000 2500;do
for cache_size in 4 8 16 32 64 128 256;do
echo "opt $cache_size" >> restore.log
i=0
while [ $i -lt $jobid ]
do
destor -r$i /home/fumin/restore/ --cache=OPT --cache_size=$cache_size --window_size=$window_size >>log
i=$(($i+1))
done
done
done
