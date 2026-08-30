// CI for the CYD stock ticker: run the host-side unit tests, then build the
// ESP32 firmware and keep the binary.
//
// Runs on the Jenkins controller (a Debian LXC on the Proxmox box) which has
// gcc/g++ and PlatformIO installed system-wide at /opt/platformio.
//
// The two PlatformIO environments do different jobs and are kept as separate
// stages so a failure says which one broke:
//   native - Unity unit tests, compiled with the host gcc. Fast, no hardware.
//   cyd    - the real ESP32 firmware image.

pipeline {
  agent any

  options {
    timestamps()
    // The ESP32 toolchain download on a cold cache is slow but not unbounded;
    // anything past this is a hang, not a slow build.
    timeout(time: 30, unit: 'MINUTES')
    disableConcurrentBuilds()
    buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
  }

  environment {
    // Keep PlatformIO's toolchain cache in the job workspace's parent so it
    // survives `cleanWs()` between builds. A cold cache costs several minutes.
    PLATFORMIO_CORE_DIR = "${JENKINS_HOME}/.platformio"
  }

  stages {
    stage('Checkout') {
      steps {
        checkout scm
        sh 'git --no-pager log -1 --oneline'
      }
    }

    stage('Unit tests (native)') {
      steps {
        // -e native builds only src/util/interpolate.cpp against Unity, per
        // build_src_filter in platformio.ini.
        sh 'pio test -e native'
      }
    }

    stage('Stub secrets.h') {
      steps {
        // src/secrets.h is gitignored (it holds the real WiFi credentials), so
        // a clean clone cannot compile without it. secrets_example.h is the
        // committed template the README tells humans to copy; CI does the same.
        //
        // This proves the code COMPILES. It does not produce a flashable image
        // for a real network - the artifact carries placeholder credentials and
        // the device is provisioned over BLE anyway.
        sh '''
          if [ ! -f src/secrets.h ]; then
            cp src/secrets_example.h src/secrets.h
            echo "secrets.h stubbed from secrets_example.h for the build"
          fi
        '''
      }
    }

    stage('Build firmware (cyd)') {
      steps {
        sh 'pio run -e cyd'
      }
    }

    stage('Archive firmware') {
      steps {
        sh 'ls -lh .pio/build/cyd/firmware.bin'
        archiveArtifacts artifacts: '.pio/build/cyd/firmware.*',
                         fingerprint: true,
                         onlyIfSuccessful: true
      }
    }
  }

  post {
    always {
      // Report the image size against the huge_app partition budget (3 MB app
      // slot). Silent growth past that only shows up as a failed flash.
      sh '''
        if [ -f .pio/build/cyd/firmware.bin ]; then
          size=$(stat -c%s .pio/build/cyd/firmware.bin)
          echo "firmware.bin = ${size} bytes ($((size / 1024)) KB) of 3072 KB app slot"
        fi
      '''
    }
    cleanup {
      cleanWs()
    }
  }
}
