if [ $# -gt 0 ]; then
echo $1 > .version
fi

make -j16

cp arch/arm/boot/zImage-dtb ../ramdisk_hammerhead/

cd ../ramdisk_hammerhead/

echo "making ramdisk"
./mkbootfs boot.img-ramdisk | gzip > ramdisk.gz
echo "making boot image"
./mkbootimg --kernel zImage-dtb --cmdline 'console=ttyHSL0,115200,n8 androidboot.hardware=hammerhead user_debug=31 msm_watchdog_v2.enable=1' --base 0x00000000 --pagesize 2048 --ramdisk_offset 0x02900000 --tags_offset 0x02700000 --ramdisk ramdisk.gz --output ../hammerhead/boot.img

rm -rf ramdisk.gz
rm -rf zImage

cd ../hammerhead/

zipfile="franco.Kernel-nightly.zip"
echo "making zip file"
cp boot.img zip/

rm -rf ../ramdisk_hammerhead/boot.img

cd zip/
rm -f *.zip
zip -r -9 $zipfile *
rm -f /tmp/*.zip
cp *.zip /tmp
