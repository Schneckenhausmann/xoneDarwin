// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "XboxControlCenter",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "XboxControlCenter", targets: ["XboxControlCenter"])
    ],
    targets: [
        .executableTarget(name: "XboxControlCenter")
    ]
)
