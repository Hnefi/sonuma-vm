#! /bin/bash

BOOT_CFG_DIR=$1

# Shutdown all domains
OLD_DOMAINS=$(xl list | sed -rn 's/^\w+\s+([0-9]+).*/\1/p')
echo "Destroying old guests...."
for i in $OLD_DOMAINS; do xl destroy $i; done
#for i in $OLD_DOMAINS; do echo "dom $i"; done

sleep 5

mv servers.txt servers.bak
echo "Creating new guests..."
declare -a IPS
FILES=$(ls $BOOT_CFG_DIR/*_boot.cfg)
for i in $FILES; do 
    #echo "create $i"; 
    xl create $i;
    IP_OF_GUEST=$(grep -oE "\b([0-9]{1,3}\.){3}[0-9]{1,3}\b" < $i);
    IPS+=($IP_OF_GUEST);
    NAME_OF_GUEST=$(sed -rn 's#^name\s*=\s*\"(\w+\d*)\"#\1#p' < $i);
    DOMID_OF_GUEST=$(xl domid $NAME_OF_GUEST);
    echo "$IP_OF_GUEST:$DOMID_OF_GUEST" >> servers.txt;
done

sleep 30

echo "Copying servers.txt to all soN guests ${IPS[@]}....";
for i in ${IPS[@]}; do
    sshpass -p "sorpc" ssh-copy-id -f sorpc@$i;
    scp servers.txt sorpc@$i:~/servers.txt;
done
