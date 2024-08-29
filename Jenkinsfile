#!/usr/bin/env groovy
@Library('pipeline@v2-stable')

import pipeline.fastly.kubernetes.jenkins.Constants
import pipeline.fastly.github.Repo
import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

import static pipeline.fastly.github.Repo.CommitStatus

final def BUILD_TIMEOUT = 120
final def NODELABEL = 'docker-build'
final def RELEASE_BRANCH = 'main'

def buildResources = [resourceRequestMemory: '92Gi', resourceLimitMemory: '92Gi', resourceRequestCpu: '25000m', resourceLimitCpu: '25000m']
def releaseBranches = [RELEASE_BRANCH, 'origin/' + RELEASE_BRANCH]
def cache = true
def cleanMergedRefs = false
def pushDeb = false
def pushImage = false
def namedBuild = ''
def tagName = null
def slackChannel = null
def emailToSlack = [
  'jkarneges@fastly.com': '@jkarneges',
  'madeline.pratt@fastly.com': '@maddie',
  'ricky.hosfelt@fastly.com': '@Ricky',
]

def configureGitCreds() {
  withCredentials([string(credentialsId: Constants.GITHUB_OAUTH_CREDENTIALS, variable: 'GITHUB_TOKEN')]) {
    // With native GITHUB_TOKEN support we want to incorporate here https://github.com/fastly/xqd_rel_notes/issues/5
    //  we don't need to create that config file
    sh(script: """
      mkdir ~/.ssh && ssh-keyscan -t rsa github.com >> ~/.ssh/known_hosts
      git config --global user.email jenkins@secretcdn.net
      git config --global user.name jenkins
      mkdir ~/.config && cat <<'EOF' >~/.config/hub
github.com:
- user: jenkins
  oauth_token: ${GITHUB_TOKEN}
  protocol: https
EOF
      """)
  }
}

// Ignore TAG push events from GitHub, only branches built
if (params.ref.contains('refs/tags/')) {
  currentBuild.result = 'ABORTED'
  currentBuild.description = 'Triggered by a TAG, ignoring ...'
  return
}

String getCleanedUpBuildRef() {
  String ref = getBuildRef().name
  def match = (ref =~ /^origin\/(.*)/)
  if (match) {
    ref = match[0][1]
  }
  return ref
}

String ref = getCleanedUpBuildRef()
if (ref in releaseBranches) {
  pushDeb = true
  pushImage = true
  cache = false
  cleanMergedRefs = true
  tagName = 'jenkins/release'
  slackChannel = '#fanout-eng'
} else if (ref =~ /^.*\/jenkins$/) {
  pushDeb = true
  cache = false
  namedBuild = ref.replaceAll('/jenkins', '').replaceAll('/', '-')
  slackChannel = emailToSlack[params.author_email]
  tagName = 'jenkins/named'
} else if (ref =~ /^.*-stable$/) {
  pushDeb = true
  cache = false
  namedBuild = ref.replaceAll('-stable', '').replaceAll('/', '-')
  slackChannel = emailToSlack[params.author_email]
  tagName = 'jenkins/stable'
} else {
  namedBuild = ref.replaceAll('/', '-')
}

fastlyPipeline(script: this, buildTimeout: BUILD_TIMEOUT, ignoreTags: true, slackChannel: slackChannel) {
  getNode(label: NODELABEL, resources: buildResources) {
    def tmpDir = pwd(tmp: true)
    checkoutWithSubmodules scm
    def v = readFile file: './fastly-build/VERSION'
    def package_version = "${v.trim()}.${env.BUILD_NUMBER}"
    if (namedBuild) {
      package_version = "0.${package_version}-${namedBuild}"
    }

    stage('Build') {
      sshagent(credentials: [Constants.GITHUB_SSHKEY_CREDENTIAL], socketPath:"${tmpDir}/agent.socket") {
        def buildContainerConfig = [
        dockerFile: 'Dockerfile',
        imageName: 'fastly/pushpin',
        pushImage: pushImage,
        cache: cache,
        additionalBuildArgs: [
          "DESTDIR=${env.WORKSPACE}",
          "PKG_VERSION=${package_version}",
          "SSH_AUTH_SOCK=${env.SSH_AUTH_SOCK}"
        ]
        ]
        fastlyDockerBuild(script: this, containers: [buildContainerConfig], checkout: false, loggerVerbosity: 'info')
      }
    }

    if (pushDeb) {
      stage('Push Packages to APT') {
        fastlyAptPush(script: this, path: env.WORKSPACE)
        if (slackChannel) {
          slackSend color: null, message: "Package `fst-pushpin` version `${package_version}` uploaded.", channel: slackChannel
        }
      }
    }

    if (tagName) {
      tagName = "${tagName}-${env.BUILD_NUMBER}-${params.commit.take(7)}"
        stage('Tag Commit') {
          tagCommit(tag: tagName)
        }
    }

    if (cleanMergedRefs) {
      stage('Cleanup Merged Refs') {
        cleanupMergedBranches(script: this, masterBranch: RELEASE_BRANCH)
      }
    }
  }
}
