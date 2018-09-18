# Variable Declarations
BASE_FOLDER=/tmp/circleci-artifacts
OPTIONS_PLIST=$BASE_FOLDER/options.plist
ARCHIVE_PATH=$BASE_FOLDER/archive.xcarchive
OUTPUT_FOLDER=$BASE_FOLDER/ipa
# Clean
rm $OPTIONS_PLIST
rm -rf $ARCHIVE_PATH
rm -rf $OUTPUT_FOLDER
#Generate .plist
PLIST='{"compileBitcode":false,"method":"ad-hoc","ProvisioningStyle": "Manual"}'
echo $PLIST | plutil -convert xml1 -o $OPTIONS_PLIST -
#Generate Archive
cd PubnativeLite
agvtool -noscm new-marketing-version "$(agvtool what-marketing-version -terse1)-${CIRCLE_BRANCH}.${CIRCLE_BUILD_NUM}"
agvtool new-version -all $CIRCLE_BUILD_NUM
cd ..
bundle exec fastlane adhoc --verbose
bundle exec fastlane gym --verbose --include_bitcode true --include_symbols true --clean --project HyBid.xcodeproj --scheme HyBid --archive_path $ARCHIVE_PATH --output_directory $OUTPUT_FOLDER --export_options $OPTIONS_PLIST
# Upload Generated IPA to Fabric
./scripts/submit $FABRIC_API_KEY $FABRIC_API_SECRET -ipaPath $OUTPUT_FOLDER/HyBid.ipa
