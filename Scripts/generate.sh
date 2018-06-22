BASE_DIR=/tmp/circleci-artifacts
PRODUCT_NAME=PubnativeLite
FRAMEWORK_NAME=$PRODUCT_NAME.framework
FAT_FRAMEWORK=$BASE_DIR/$FRAMEWORK_NAME
FAT_PRODUCT=$FAT_FRAMEWORK/$PRODUCT_NAME
FAT_ZIP_PATH=$BASE_DIR/PubnativeLite.framework.zip
IPHONEOS_PATH=$BASE_DIR/iphoneos
IPHONEOS_ARCH=$IPHONEOS_PATH/arch
IPHONEOS_FRAMEWORK=$IPHONEOS_PATH/$FRAMEWORK_NAME
IPHONEOS_PRODUCT=$IPHONEOS_FRAMEWORK/$PRODUCT_NAME
IPHONEOS_ZIP_PATH=$BASE_DIR/PubnativeLite.iphoneos.framework.zip
IPHONESIMULATOR_PATH=$BASE_DIR/iphonesimulator
IPHONESIMULATOR_FRAMEWORK=$IPHONESIMULATOR_PATH/$FRAMEWORK_NAME
IPHONESIMULATOR_PRODUCT=$IPHONESIMULATOR_FRAMEWORK/$PRODUCT_NAME
IPHONESIMULATOR_ZIP_PATH=$BASE_DIR/PubnativeLite.iphonesimulator.framework.zip

# GENERATE
xcodebuild -workspace PubnativeLite.xcworkspace -scheme PubnativeLite -sdk iphoneos -configuration Release clean build CODE_SIGN_IDENTITY="iPhone Distribution" CODE_SIGNING_REQUIRED=NO CONFIGURATION_BUILD_DIR=$IPHONEOS_PATH | xcpretty -c
xcodebuild -workspace PubnativeLite.xcworkspace -scheme PubnativeLite -sdk iphonesimulator -configuration Release clean build CONFIGURATION_BUILD_DIR=$IPHONESIMULATOR_PATH | xcpretty -c

# MERGE
cp -rf $IPHONEOS_FRAMEWORK $FAT_FRAMEWORK
rm $FAT_PRODUCT
lipo -create $IPHONEOS_PRODUCT $IPHONESIMULATOR_PRODUCT -output $FAT_PRODUCT
zip -r $FAT_ZIP_PATH $FAT_FRAMEWORK
zip -r $IPHONEOS_ZIP_PATH $IPHONEOS_FRAMEWORK
zip -r $IPHONESIMULATOR_ZIP_PATH $IPHONESIMULATOR_FRAMEWORK
