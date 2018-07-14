#! /bin/bash

BOOT_CFG_DIR=$1

# Shutdown all domains
OLD_DOMAINS=$(xl list | sed -rn 's/^\w+\s+([0-9]+).*/\1/p')
echo "Destroying old guests...."
for i in $OLD_DOMAINS; do xl destroy $i; done

sleep 5

mv servers.txt servers.bak
echo "Creating new guests..."
declare -a IPS
FILES=$(ls $BOOT_CFG_DIR/*_boot.cfg)
let count=0
for i in $FILES; do 
    echo "create $i"; 
    xl create $i;
    IP_OF_GUEST=$(grep -oE "\b([0-9]{1,3}\.){3}[0-9]{1,3}\b" < $i);
    IPS+=($IP_OF_GUEST);
    NAME_OF_GUEST=$(sed -rn 's#^name\s*=\s*\"(\w+\d*)\"#\1#p' < $i);
    DOMID_OF_GUEST=$(xl domid $NAME_OF_GUEST);
    echo "$count:$IP_OF_GUEST:$DOMID_OF_GUEST" >> servers.txt;
    ((count+=1))
    echo ${IPS[@]};
done

sleep 30

# in order for this to work, you need to set up /etc/sshd/sshd_config on the VMs with
# all of the following manully (just once when you create the images):
#   PasswordAuthentication yes
#   PermitRootLogin yes
#   $ service sshd restart
echo "Copying servers.txt to all soN guests ${IPS[@]}....";
for i in ${IPS[@]}; do
    sshpass -p "root" ssh-copy-id -f root@$i;
    scp servers.txt root@$i:~/servers.txt;
done
